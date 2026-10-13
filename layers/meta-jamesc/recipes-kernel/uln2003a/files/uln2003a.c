// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/wait.h>

/* Bits correspond to IN1..IN4; eight phases per half-step cycle. */
static const unsigned char phases[] = { 1, 3, 2, 6, 4, 12, 8, 9 };

struct uln2003a {
	struct gpio_descs *gpios;
	struct task_struct *thread;
	struct mutex lock;
	wait_queue_head_t wait;
	u32 remaining, completed, speed, generation;
	int phase, direction;
	bool busy, reverse;
};

static void coils(struct uln2003a *motor, unsigned long value)
{
	gpiod_set_array_value_cansleep(4, motor->gpios->desc,
				      motor->gpios->info, &value);
}

static int motor_thread(void *data)
{
	struct uln2003a *motor = data;
	u32 generation, speed;

	while (!kthread_should_stop()) {
		wait_event_interruptible(motor->wait,
			READ_ONCE(motor->busy) || kthread_should_stop());
		if (kthread_should_stop())
			break;
		mutex_lock(&motor->lock);
		if (!motor->busy) {
			mutex_unlock(&motor->lock);
			continue;
		}
		generation = motor->generation;
		speed = motor->speed;
		motor->phase = (motor->phase + motor->direction + 8) % 8;
		coils(motor, phases[motor->phase]);
		motor->remaining--;
		motor->completed++;
		mutex_unlock(&motor->lock);

		/* Sleepable GPIOs run in process context. Stop wakes this delay. */
		wait_event_interruptible_hrtimeout(motor->wait,
			kthread_should_stop() ||
			READ_ONCE(motor->generation) != generation,
			ns_to_ktime(NSEC_PER_SEC / speed));
		mutex_lock(&motor->lock);
		if (motor->generation == generation && !motor->remaining) {
			coils(motor, 0);
			motor->busy = false;
		}
		mutex_unlock(&motor->lock);
	}
	coils(motor, 0);
	return 0;
}

/* One atomic command: direction, half-steps/second, half-step count. */
static ssize_t move_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct uln2003a *motor = dev_get_drvdata(dev);
	char direction[5], extra;
	unsigned int speed, steps;
	int sign;

	if (sscanf(buf, "%4s %u %u %c", direction, &speed, &steps, &extra) != 3)
		return -EINVAL;
	if (!strcmp(direction, "cw"))
		sign = 1;
	else if (!strcmp(direction, "acw"))
		sign = -1;
	else
		return -EINVAL;
	if (!speed || speed > 1000 || !steps || steps > 1000000)
		return -ERANGE;
	mutex_lock(&motor->lock);
	if (motor->busy) {
		mutex_unlock(&motor->lock);
		return -EBUSY;
	}
	motor->direction = motor->reverse ? -sign : sign;
	motor->speed = speed;
	motor->remaining = steps;
	motor->completed = 0;
	motor->generation++;
	motor->busy = true;
	mutex_unlock(&motor->lock);
	wake_up(&motor->wait);
	return count;
}
static DEVICE_ATTR_WO(move);

static ssize_t stop_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct uln2003a *motor = dev_get_drvdata(dev);
	bool stop;

	if (kstrtobool(buf, &stop) || !stop)
		return -EINVAL;
	mutex_lock(&motor->lock);
	motor->busy = false;
	motor->remaining = 0;
	motor->generation++;
	coils(motor, 0);
	mutex_unlock(&motor->lock);
	wake_up(&motor->wait);
	return count;
}
static DEVICE_ATTR_WO(stop);

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct uln2003a *motor = dev_get_drvdata(dev);
	ssize_t len;
	int direction;

	mutex_lock(&motor->lock);
	direction = motor->reverse ? -motor->direction : motor->direction;
	len = sysfs_emit(buf,
		"running=%u direction=%s speed=%u completed=%u remaining=%u\n",
		motor->busy, direction == 1 ? "clockwise" : "anticlockwise",
		motor->speed, motor->completed, motor->remaining);
	mutex_unlock(&motor->lock);
	return len;
}
static DEVICE_ATTR_RO(status);

static struct attribute *motor_attrs[] = {
	&dev_attr_move.attr, &dev_attr_stop.attr, &dev_attr_status.attr, NULL,
};
static const struct attribute_group motor_group = { .attrs = motor_attrs };

static int motor_probe(struct platform_device *pdev)
{
	struct uln2003a *motor;
	int ret;

	motor = devm_kzalloc(&pdev->dev, sizeof(*motor), GFP_KERNEL);
	if (!motor)
		return -ENOMEM;
	motor->gpios = devm_gpiod_get_array(&pdev->dev, "coil", GPIOD_OUT_LOW);
	if (IS_ERR(motor->gpios))
		return dev_err_probe(&pdev->dev, PTR_ERR(motor->gpios),
				     "cannot acquire coil GPIOs\n");
	if (motor->gpios->ndescs != 4)
		return dev_err_probe(&pdev->dev, -EINVAL, "need four coil GPIOs\n");
	motor->reverse = device_property_read_bool(&pdev->dev, "reverse-direction");
	motor->direction = motor->reverse ? -1 : 1;
	motor->phase = 7;
	mutex_init(&motor->lock);
	init_waitqueue_head(&motor->wait);
	platform_set_drvdata(pdev, motor);
	motor->thread = kthread_run(motor_thread, motor, "uln2003a-%s",
				    dev_name(&pdev->dev));
	if (IS_ERR(motor->thread))
		return PTR_ERR(motor->thread);
	ret = sysfs_create_group(&pdev->dev.kobj, &motor_group);
	if (ret)
		kthread_stop(motor->thread);
	return ret;
}

static void motor_remove(struct platform_device *pdev)
{
	struct uln2003a *motor = platform_get_drvdata(pdev);

	sysfs_remove_group(&pdev->dev.kobj, &motor_group);
	kthread_stop(motor->thread);
}

static void motor_shutdown(struct platform_device *pdev)
{
	struct uln2003a *motor = platform_get_drvdata(pdev);

	kthread_stop(motor->thread);
}

static const struct of_device_id motor_of_match[] = {
	{ .compatible = "jamesc,uln2003a-stepper" }, { }
};
MODULE_DEVICE_TABLE(of, motor_of_match);

static struct platform_driver motor_driver = {
	.probe = motor_probe,
	.remove_new = motor_remove,
	.shutdown = motor_shutdown,
	.driver = {
		.name = "uln2003a",
		.of_match_table = motor_of_match,
	},
};
module_platform_driver(motor_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Four-coil ULN2003A half-step motor driver");
