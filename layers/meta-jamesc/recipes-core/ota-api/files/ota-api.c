#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <microhttpd.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define DEFAULT_BIND "127.0.0.1"
#define DEFAULT_PORT 8080
#define DEFAULT_RAUC_BIN "/usr/bin/rauc"
#define DEFAULT_TOKEN_FILE "/data/ota/api-token"
#define DEFAULT_UPLOAD_DIR "/data/ota"
#define DEFAULT_MAX_UPLOAD (384ULL * 1024ULL * 1024ULL)
#define TOKEN_MAX 256
#define VALUE_MAX 256

enum update_phase {
    UPDATE_IDLE,
    UPDATE_UPLOADING,
    UPDATE_INSTALLING,
    UPDATE_SUCCEEDED,
    UPDATE_FAILED,
};

struct server_state {
    pthread_mutex_t lock;
    char token[TOKEN_MAX];
    char rauc_bin[PATH_MAX];
    char upload_dir[PATH_MAX];
    char part_path[PATH_MAX];
    char bundle_path[PATH_MAX];
    uint64_t max_upload;
    enum update_phase phase;
    uint64_t bytes_received;
    pid_t installer_pid;
    int installer_exit;
    bool upload_active;
};

struct request_state {
    struct server_state *server;
    int fd;
    unsigned int error_status;
    uint64_t received;
    bool authenticated;
    bool upload;
    bool owns_upload;
    bool finalized;
};

static volatile sig_atomic_t stopping;

static int write_all(int fd, const char *data, size_t size);

static const char *phase_name(enum update_phase phase)
{
    switch (phase) {
    case UPDATE_IDLE:
        return "idle";
    case UPDATE_UPLOADING:
        return "uploading";
    case UPDATE_INSTALLING:
        return "installing";
    case UPDATE_SUCCEEDED:
        return "succeeded";
    case UPDATE_FAILED:
        return "failed";
    }
    return "unknown";
}

static void handle_signal(int signo)
{
    (void)signo;
    stopping = 1;
}

static int parse_u16(const char *text, uint16_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0 || parsed > 65535)
        return -1;
    *value = (uint16_t)parsed;
    return 0;
}

static int parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0)
        return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static int read_text_file(const char *path, char *buffer, size_t size)
{
    FILE *file;
    size_t length;

    if (size == 0)
        return -1;
    file = fopen(path, "re");
    if (file == NULL)
        return -1;
    if (fgets(buffer, (int)size, file) == NULL) {
        fclose(file);
        return -1;
    }
    fclose(file);

    length = strlen(buffer);
    while (length > 0 && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r' ||
                          buffer[length - 1] == ' ' || buffer[length - 1] == '\t'))
        buffer[--length] = '\0';
    return length > 0 ? 0 : -1;
}

static void json_escape(const char *input, char *output, size_t size)
{
    static const char hex[] = "0123456789abcdef";
    size_t used = 0;

    if (size == 0)
        return;
    while (*input != '\0' && used + 1 < size) {
        unsigned char c = (unsigned char)*input++;

        if (c == '"' || c == '\\') {
            if (used + 2 >= size)
                break;
            output[used++] = '\\';
            output[used++] = (char)c;
        } else if (c < 0x20) {
            if (used + 6 >= size)
                break;
            output[used++] = '\\';
            output[used++] = 'u';
            output[used++] = '0';
            output[used++] = '0';
            output[used++] = hex[c >> 4];
            output[used++] = hex[c & 0xf];
        } else {
            output[used++] = (char)c;
        }
    }
    output[used] = '\0';
}

static void os_release_value(const char *key, char *value, size_t size)
{
    FILE *file;
    char line[512];
    size_t key_length = strlen(key);

    value[0] = '\0';
    file = fopen("/etc/os-release", "re");
    if (file == NULL)
        return;
    while (fgets(line, sizeof(line), file) != NULL) {
        char *start;
        char *end;

        if (strncmp(line, key, key_length) != 0 || line[key_length] != '=')
            continue;
        start = line + key_length + 1;
        end = start + strcspn(start, "\r\n");
        *end = '\0';
        if (*start == '"') {
            start++;
            end = strrchr(start, '"');
            if (end != NULL)
                *end = '\0';
        }
        snprintf(value, size, "%s", start);
        break;
    }
    fclose(file);
}

static void cmdline_value(const char *key, char *value, size_t size)
{
    FILE *file;
    char *line = NULL;
    char *save = NULL;
    char *item;
    size_t allocated = 0;
    size_t key_length = strlen(key);

    value[0] = '\0';
    file = fopen("/proc/cmdline", "re");
    if (file == NULL)
        return;
    if (getline(&line, &allocated, file) < 0) {
        fclose(file);
        free(line);
        return;
    }
    fclose(file);

    for (item = strtok_r(line, " \r\n", &save); item != NULL;
         item = strtok_r(NULL, " \r\n", &save)) {
        if (strncmp(item, key, key_length) == 0 && item[key_length] == '=') {
            snprintf(value, size, "%s", item + key_length + 1);
            break;
        }
    }
    free(line);
}

static void cpu_model(char *value, size_t size)
{
    FILE *file;
    char line[512];

    value[0] = '\0';
    file = fopen("/proc/cpuinfo", "re");
    if (file != NULL) {
        while (fgets(line, sizeof(line), file) != NULL) {
            char *colon;
            char *start;
            char *end;

            if (strncmp(line, "model name", 10) != 0 &&
                strncmp(line, "Model", 5) != 0 &&
                strncmp(line, "Hardware", 8) != 0)
                continue;
            colon = strchr(line, ':');
            if (colon == NULL)
                continue;
            start = colon + 1;
            while (*start == ' ' || *start == '\t')
                start++;
            end = start + strcspn(start, "\r\n");
            *end = '\0';
            snprintf(value, size, "%s", start);
            break;
        }
        fclose(file);
    }
    if (value[0] == '\0' && read_text_file("/proc/device-tree/model", value, size) != 0)
        snprintf(value, size, "unknown");
}

static void memory_info(uint64_t *total_bytes, uint64_t *available_bytes)
{
    FILE *file;
    char line[256];
    unsigned long long total_kib = 0;
    unsigned long long available_kib = 0;

    file = fopen("/proc/meminfo", "re");
    if (file != NULL) {
        while (fgets(line, sizeof(line), file) != NULL) {
            if (sscanf(line, "MemTotal: %llu kB", &total_kib) == 1)
                continue;
            (void)sscanf(line, "MemAvailable: %llu kB", &available_kib);
        }
        fclose(file);
    }
    *total_bytes = (uint64_t)total_kib * 1024ULL;
    *available_bytes = (uint64_t)available_kib * 1024ULL;
}

static void build_id_time(const char *build_id, char *value, size_t size)
{
    unsigned int year, month, day, hour, minute, second;

    value[0] = '\0';
    if (strlen(build_id) == 14 &&
        sscanf(build_id, "%4u%2u%2u%2u%2u%2u", &year, &month, &day,
               &hour, &minute, &second) == 6) {
        snprintf(value, size, "%04u-%02u-%02uT%02u:%02u:%02uZ", year, month,
                 day, hour, minute, second);
    }
}

static void record_successful_update(const struct server_state *server)
{
    char path[PATH_MAX];
    char temporary[PATH_MAX];
    char timestamp[32];
    struct tm utc;
    time_t now;
    int fd;
    size_t length;

    if (snprintf(path, sizeof(path), "%s/last-successful-update", server->upload_dir) >=
            (int)sizeof(path) ||
        snprintf(temporary, sizeof(temporary), "%s.tmp", path) >=
            (int)sizeof(temporary))
        return;
    now = time(NULL);
    if (now == (time_t)-1 || gmtime_r(&now, &utc) == NULL ||
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ\n", &utc) == 0)
        return;
    fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    length = strlen(timestamp);
    if (write_all(fd, timestamp, length) != 0 || fsync(fd) != 0) {
        close(fd);
        unlink(temporary);
        return;
    }
    if (close(fd) != 0 || rename(temporary, path) != 0)
        unlink(temporary);
}

static enum MHD_Result queue_json(struct MHD_Connection *connection,
                                  unsigned int status,
                                  const char *json)
{
    struct MHD_Response *response;
    enum MHD_Result result;

    response = MHD_create_response_from_buffer(strlen(json), (void *)json,
                                               MHD_RESPMEM_MUST_COPY);
    if (response == NULL)
        return MHD_NO;
    MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE,
                            "application/json; charset=utf-8");
    MHD_add_response_header(response, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
    result = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return result;
}

static bool constant_time_equal(const char *left, const char *right)
{
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    size_t maximum = left_length > right_length ? left_length : right_length;
    unsigned int difference = (unsigned int)(left_length ^ right_length);
    size_t index;

    for (index = 0; index < maximum; index++) {
        unsigned char a = index < left_length ? (unsigned char)left[index] : 0;
        unsigned char b = index < right_length ? (unsigned char)right[index] : 0;
        difference |= a ^ b;
    }
    return difference == 0;
}

static bool is_authenticated(struct MHD_Connection *connection,
                             const struct server_state *server)
{
    const char *header = MHD_lookup_connection_value(connection, MHD_HEADER_KIND,
                                                     MHD_HTTP_HEADER_AUTHORIZATION);
    char expected[TOKEN_MAX + 8];

    if (header == NULL)
        return false;
    if (snprintf(expected, sizeof(expected), "Bearer %s", server->token) >=
        (int)sizeof(expected))
        return false;
    return constant_time_equal(header, expected);
}

static enum MHD_Result queue_unauthorized(struct MHD_Connection *connection)
{
    struct MHD_Response *response;
    const char body[] = "{\"error\":\"unauthorized\"}\n";
    enum MHD_Result result;

    response = MHD_create_response_from_buffer(sizeof(body) - 1, (void *)body,
                                               MHD_RESPMEM_MUST_COPY);
    if (response == NULL)
        return MHD_NO;
    MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE,
                            "application/json; charset=utf-8");
    MHD_add_response_header(response, MHD_HTTP_HEADER_WWW_AUTHENTICATE,
                            "Bearer realm=\"ota-api\"");
    result = MHD_queue_response(connection, MHD_HTTP_UNAUTHORIZED, response);
    MHD_destroy_response(response);
    return result;
}

static enum MHD_Result system_response(struct MHD_Connection *connection,
                                      const struct server_state *server)
{
    struct utsname uts;
    char hostname[VALUE_MAX] = "unknown";
    char pretty[VALUE_MAX] = "unknown";
    char version[VALUE_MAX] = "unknown";
    char build_id[VALUE_MAX] = "unknown";
    char built_at[VALUE_MAX] = "unknown";
    char updated_at[VALUE_MAX] = "unknown";
    char model[VALUE_MAX] = "unknown";
    char slot[VALUE_MAX] = "unknown";
    char root[VALUE_MAX] = "unknown";
    char escaped_hostname[VALUE_MAX * 2];
    char escaped_pretty[VALUE_MAX * 2];
    char escaped_version[VALUE_MAX * 2];
    char escaped_build_id[VALUE_MAX * 2];
    char escaped_built_at[VALUE_MAX * 2];
    char escaped_updated_at[VALUE_MAX * 2];
    char escaped_model[VALUE_MAX * 2];
    char escaped_slot[VALUE_MAX * 2];
    char escaped_root[VALUE_MAX * 2];
    char escaped_release[VALUE_MAX * 2];
    char escaped_machine[VALUE_MAX * 2];
    char uptime_text[VALUE_MAX] = "0";
    char load_text[VALUE_MAX] = "0 0 0";
    char update_time_path[PATH_MAX];
    char json[8192];
    double uptime = 0.0;
    double load_1m = 0.0;
    double load_5m = 0.0;
    double load_15m = 0.0;
    double memory_percent = 0.0;
    uint64_t memory_total = 0;
    uint64_t memory_available = 0;
    uint64_t memory_used = 0;
    long cores;

    memset(&uts, 0, sizeof(uts));
    if (uname(&uts) != 0) {
        snprintf(uts.release, sizeof(uts.release), "unknown");
        snprintf(uts.machine, sizeof(uts.machine), "unknown");
    }
    if (gethostname(hostname, sizeof(hostname)) != 0)
        snprintf(hostname, sizeof(hostname), "unknown");
    hostname[sizeof(hostname) - 1] = '\0';
    os_release_value("PRETTY_NAME", pretty, sizeof(pretty));
    os_release_value("VERSION_ID", version, sizeof(version));
    os_release_value("BUILD_ID", build_id, sizeof(build_id));
    if (pretty[0] == '\0')
        snprintf(pretty, sizeof(pretty), "unknown");
    if (version[0] == '\0')
        snprintf(version, sizeof(version), "unknown");
    if (build_id[0] == '\0')
        snprintf(build_id, sizeof(build_id), "unknown");
    build_id_time(build_id, built_at, sizeof(built_at));
    if (built_at[0] == '\0')
        snprintf(built_at, sizeof(built_at), "unknown");
    if (snprintf(update_time_path, sizeof(update_time_path),
                 "%s/last-successful-update", server->upload_dir) >=
            (int)sizeof(update_time_path) ||
        read_text_file(update_time_path, updated_at, sizeof(updated_at)) != 0)
        snprintf(updated_at, sizeof(updated_at), "%s", built_at);
    cpu_model(model, sizeof(model));
    memory_info(&memory_total, &memory_available);
    if (memory_available > memory_total)
        memory_available = memory_total;
    memory_used = memory_total - memory_available;
    if (memory_total != 0)
        memory_percent = (double)memory_used * 100.0 / (double)memory_total;
    cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 1)
        cores = 1;
    cmdline_value("rauc.slot", slot, sizeof(slot));
    cmdline_value("root", root, sizeof(root));
    if (read_text_file("/proc/uptime", uptime_text, sizeof(uptime_text)) == 0)
        (void)sscanf(uptime_text, "%lf", &uptime);
    if (read_text_file("/proc/loadavg", load_text, sizeof(load_text)) == 0)
        (void)sscanf(load_text, "%lf %lf %lf", &load_1m, &load_5m, &load_15m);

    json_escape(hostname, escaped_hostname, sizeof(escaped_hostname));
    json_escape(pretty, escaped_pretty, sizeof(escaped_pretty));
    json_escape(version, escaped_version, sizeof(escaped_version));
    json_escape(build_id, escaped_build_id, sizeof(escaped_build_id));
    json_escape(built_at, escaped_built_at, sizeof(escaped_built_at));
    json_escape(updated_at, escaped_updated_at, sizeof(escaped_updated_at));
    json_escape(model, escaped_model, sizeof(escaped_model));
    json_escape(slot, escaped_slot, sizeof(escaped_slot));
    json_escape(root, escaped_root, sizeof(escaped_root));
    json_escape(uts.release, escaped_release, sizeof(escaped_release));
    json_escape(uts.machine, escaped_machine, sizeof(escaped_machine));

    snprintf(json, sizeof(json),
             "{\"hostname\":\"%s\",\"os\":\"%s\","
             "\"firmware\":{\"version\":\"%s\",\"build_id\":\"%s\","
             "\"built_at\":\"%s\",\"updated_at\":\"%s\"},"
             "\"cpu\":{\"model\":\"%s\",\"architecture\":\"%s\","
             "\"logical_cores\":%ld,\"load_1m\":%.2f,\"load_5m\":%.2f,"
             "\"load_15m\":%.2f},"
             "\"memory\":{\"total_bytes\":%llu,\"available_bytes\":%llu,"
             "\"used_bytes\":%llu,\"usage_percent\":%.1f},"
             "\"kernel\":\"%s\",\"rauc_slot\":\"%s\",\"root\":\"%s\","
             "\"uptime_seconds\":%.0f}\n",
             escaped_hostname, escaped_pretty, escaped_version, escaped_build_id,
             escaped_built_at, escaped_updated_at, escaped_model, escaped_machine,
             cores, load_1m, load_5m, load_15m,
             (unsigned long long)memory_total,
             (unsigned long long)memory_available,
             (unsigned long long)memory_used, memory_percent, escaped_release,
             escaped_slot, escaped_root, uptime);
    return queue_json(connection, MHD_HTTP_OK, json);
}

static enum MHD_Result update_status_response(struct MHD_Connection *connection,
                                              struct server_state *server)
{
    enum update_phase phase;
    uint64_t received;
    int exit_code;
    char json[512];

    pthread_mutex_lock(&server->lock);
    phase = server->phase;
    received = server->bytes_received;
    exit_code = server->installer_exit;
    pthread_mutex_unlock(&server->lock);

    if (phase == UPDATE_FAILED) {
        snprintf(json, sizeof(json),
                 "{\"state\":\"%s\",\"bytes_received\":%llu,\"exit_code\":%d}\n",
                 phase_name(phase), (unsigned long long)received, exit_code);
    } else {
        snprintf(json, sizeof(json),
                 "{\"state\":\"%s\",\"bytes_received\":%llu}\n",
                 phase_name(phase), (unsigned long long)received);
    }
    return queue_json(connection, MHD_HTTP_OK, json);
}

static int write_all(int fd, const char *data, size_t size)
{
    while (size > 0) {
        ssize_t written = write(fd, data, size);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        data += written;
        size -= (size_t)written;
    }
    return 0;
}

static enum MHD_Result finish_upload(struct MHD_Connection *connection,
                                     struct request_state *request)
{
    struct server_state *server = request->server;
    char *argv[] = {"rauc", "install", server->bundle_path, NULL};
    pid_t pid;
    int spawn_result;
    int store_error = 0;

    if (request->received == 0)
        return queue_json(connection, MHD_HTTP_BAD_REQUEST,
                          "{\"error\":\"empty bundle\"}\n");
    if (fsync(request->fd) != 0)
        store_error = errno;
    if (close(request->fd) != 0 && store_error == 0)
        store_error = errno;
    if (store_error != 0) {
        request->fd = -1;
        return queue_json(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
                          "{\"error\":\"failed to store bundle\"}\n");
    }
    request->fd = -1;
    if (rename(server->part_path, server->bundle_path) != 0)
        return queue_json(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
                          "{\"error\":\"failed to finalize bundle\"}\n");

    spawn_result = posix_spawn(&pid, server->rauc_bin, NULL, NULL, argv, environ);
    if (spawn_result != 0) {
        pthread_mutex_lock(&server->lock);
        server->phase = UPDATE_FAILED;
        server->installer_exit = spawn_result;
        server->upload_active = false;
        pthread_mutex_unlock(&server->lock);
        request->finalized = true;
        return queue_json(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
                          "{\"error\":\"failed to start RAUC\"}\n");
    }

    pthread_mutex_lock(&server->lock);
    server->phase = UPDATE_INSTALLING;
    server->installer_pid = pid;
    server->installer_exit = 0;
    server->upload_active = false;
    pthread_mutex_unlock(&server->lock);
    request->finalized = true;
    return queue_json(connection, MHD_HTTP_ACCEPTED,
                      "{\"state\":\"installing\"}\n");
}

static enum MHD_Result handle_upload(struct MHD_Connection *connection,
                                     const char *upload_data,
                                     size_t *upload_data_size,
                                     struct request_state *request)
{
    struct server_state *server = request->server;

    if (request->error_status != 0) {
        unsigned int status = request->error_status;

        request->error_status = 0;
        return queue_json(connection, status,
                          status == MHD_HTTP_CONTENT_TOO_LARGE
                              ? "{\"error\":\"bundle exceeds upload limit\"}\n"
                              : "{\"error\":\"failed to store bundle\"}\n");
    }

    if (*upload_data_size != 0) {
        size_t chunk = *upload_data_size;

        if (request->fd < 0) {
            request->fd = open(server->part_path,
                               O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (request->fd < 0) {
                request->error_status = MHD_HTTP_INTERNAL_SERVER_ERROR;
                *upload_data_size = 0;
                return MHD_YES;
            }
        }
        if (request->received > server->max_upload ||
            chunk > server->max_upload - request->received) {
            request->error_status = MHD_HTTP_CONTENT_TOO_LARGE;
            *upload_data_size = 0;
            return MHD_YES;
        }
        if (write_all(request->fd, upload_data, chunk) != 0) {
            request->error_status = MHD_HTTP_INTERNAL_SERVER_ERROR;
            *upload_data_size = 0;
            return MHD_YES;
        }
        request->received += chunk;
        pthread_mutex_lock(&server->lock);
        server->bytes_received = request->received;
        pthread_mutex_unlock(&server->lock);
        *upload_data_size = 0;
        return MHD_YES;
    }

    return finish_upload(connection, request);
}

static enum MHD_Result request_handler(void *cls,
                                       struct MHD_Connection *connection,
                                       const char *url,
                                       const char *method,
                                       const char *version,
                                       const char *upload_data,
                                       size_t *upload_data_size,
                                       void **request_cls)
{
    struct server_state *server = cls;
    struct request_state *request = *request_cls;

    (void)version;
    if (request == NULL) {
        const char *length_header;
        uint64_t content_length = 0;
        bool busy = false;

        request = calloc(1, sizeof(*request));
        if (request == NULL)
            return MHD_NO;
        request->server = server;
        request->fd = -1;
        request->authenticated = is_authenticated(connection, server);
        request->upload = strcmp(method, MHD_HTTP_METHOD_POST) == 0 &&
                          strcmp(url, "/api/v1/update") == 0;

        if (request->upload && request->authenticated) {
            length_header = MHD_lookup_connection_value(connection, MHD_HEADER_KIND,
                                                        MHD_HTTP_HEADER_CONTENT_LENGTH);
            if (length_header != NULL &&
                (parse_u64(length_header, &content_length) != 0 ||
                 content_length > server->max_upload)) {
                request->error_status = MHD_HTTP_CONTENT_TOO_LARGE;
            } else {
                pthread_mutex_lock(&server->lock);
                busy = server->upload_active || server->phase == UPDATE_INSTALLING;
                if (!busy) {
                    server->upload_active = true;
                    server->phase = UPDATE_UPLOADING;
                    server->bytes_received = 0;
                    server->installer_exit = 0;
                    request->owns_upload = true;
                }
                pthread_mutex_unlock(&server->lock);
                if (busy)
                    request->error_status = MHD_HTTP_CONFLICT;
            }
        }
        *request_cls = request;
        return MHD_YES;
    }

    if (strcmp(method, MHD_HTTP_METHOD_GET) == 0 &&
        strcmp(url, "/api/v1/health") == 0)
        return queue_json(connection, MHD_HTTP_OK, "{\"status\":\"ok\"}\n");

    if (!request->authenticated)
        return queue_unauthorized(connection);

    if (strcmp(method, MHD_HTTP_METHOD_GET) == 0 &&
        strcmp(url, "/api/v1/system") == 0)
        return system_response(connection, server);

    if (strcmp(method, MHD_HTTP_METHOD_GET) == 0 &&
        strcmp(url, "/api/v1/update") == 0)
        return update_status_response(connection, server);

    if (request->upload) {
        if (request->error_status == MHD_HTTP_CONFLICT) {
            request->error_status = 0;
            return queue_json(connection, MHD_HTTP_CONFLICT,
                              "{\"error\":\"another update is active\"}\n");
        }
        return handle_upload(connection, upload_data, upload_data_size, request);
    }

    return queue_json(connection, MHD_HTTP_NOT_FOUND,
                      "{\"error\":\"not found\"}\n");
}

static void request_completed(void *cls,
                              struct MHD_Connection *connection,
                              void **request_cls,
                              enum MHD_RequestTerminationCode termination)
{
    struct request_state *request = *request_cls;

    (void)cls;
    (void)connection;
    (void)termination;
    if (request == NULL)
        return;
    if (request->fd >= 0)
        close(request->fd);
    if (request->owns_upload && !request->finalized) {
        unlink(request->server->part_path);
        pthread_mutex_lock(&request->server->lock);
        request->server->upload_active = false;
        if (request->server->phase == UPDATE_UPLOADING)
            request->server->phase = UPDATE_FAILED;
        pthread_mutex_unlock(&request->server->lock);
    }
    free(request);
    *request_cls = NULL;
}

static void reap_installer(struct server_state *server)
{
    int status;
    pid_t result;
    bool succeeded = false;

    pthread_mutex_lock(&server->lock);
    if (server->phase != UPDATE_INSTALLING || server->installer_pid <= 0) {
        pthread_mutex_unlock(&server->lock);
        return;
    }
    result = waitpid(server->installer_pid, &status, WNOHANG);
    if (result == server->installer_pid) {
        if (WIFEXITED(status))
            server->installer_exit = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            server->installer_exit = 128 + WTERMSIG(status);
        else
            server->installer_exit = 1;
        server->phase = server->installer_exit == 0 ? UPDATE_SUCCEEDED : UPDATE_FAILED;
        succeeded = server->phase == UPDATE_SUCCEEDED;
        server->installer_pid = 0;
    }
    pthread_mutex_unlock(&server->lock);
    if (succeeded)
        record_successful_update(server);
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--bind ADDRESS] [--port PORT] [--token-file PATH] "
            "[--upload-dir PATH] [--max-upload BYTES] [--rauc-bin PATH]\n",
            program);
}

int main(int argc, char **argv)
{
    struct server_state server = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .max_upload = DEFAULT_MAX_UPLOAD,
        .phase = UPDATE_IDLE,
        .installer_exit = 0,
    };
    const char *bind_address = DEFAULT_BIND;
    const char *rauc_bin = DEFAULT_RAUC_BIN;
    const char *token_file = DEFAULT_TOKEN_FILE;
    const char *upload_dir = DEFAULT_UPLOAD_DIR;
    struct sockaddr_in address;
    struct MHD_Daemon *daemon;
    struct sigaction action;
    uint16_t port = DEFAULT_PORT;
    int index;

    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--bind") == 0 && index + 1 < argc) {
            bind_address = argv[++index];
        } else if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            if (parse_u16(argv[++index], &port) != 0) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[index], "--token-file") == 0 && index + 1 < argc) {
            token_file = argv[++index];
        } else if (strcmp(argv[index], "--upload-dir") == 0 && index + 1 < argc) {
            upload_dir = argv[++index];
        } else if (strcmp(argv[index], "--max-upload") == 0 && index + 1 < argc) {
            if (parse_u64(argv[++index], &server.max_upload) != 0) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[index], "--rauc-bin") == 0 && index + 1 < argc) {
            rauc_bin = argv[++index];
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (read_text_file(token_file, server.token, sizeof(server.token)) != 0 ||
        strlen(server.token) < 16) {
        fprintf(stderr, "failed to read a valid token from %s\n", token_file);
        return EXIT_FAILURE;
    }
    if (snprintf(server.rauc_bin, sizeof(server.rauc_bin), "%s", rauc_bin) >=
            (int)sizeof(server.rauc_bin) ||
        snprintf(server.upload_dir, sizeof(server.upload_dir), "%s", upload_dir) >=
            (int)sizeof(server.upload_dir) ||
        snprintf(server.part_path, sizeof(server.part_path), "%s/update.raucb.part",
                 upload_dir) >= (int)sizeof(server.part_path) ||
        snprintf(server.bundle_path, sizeof(server.bundle_path), "%s/update.raucb",
                 upload_dir) >= (int)sizeof(server.bundle_path)) {
        fprintf(stderr, "upload directory path is too long\n");
        return EXIT_FAILURE;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_address, &address.sin_addr) != 1) {
        fprintf(stderr, "invalid IPv4 bind address: %s\n", bind_address);
        return EXIT_FAILURE;
    }

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    daemon = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD, port, NULL, NULL,
                              request_handler, &server,
                              MHD_OPTION_SOCK_ADDR, &address,
                              MHD_OPTION_NOTIFY_COMPLETED, request_completed, NULL,
                              MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int)30,
                              MHD_OPTION_PER_IP_CONNECTION_LIMIT, (unsigned int)4,
                              MHD_OPTION_END);
    if (daemon == NULL) {
        fprintf(stderr, "failed to listen on %s:%u\n", bind_address, port);
        return EXIT_FAILURE;
    }

    fprintf(stderr, "OTA API listening on %s:%u\n", bind_address, port);
    while (!stopping) {
        struct timespec delay = {.tv_sec = 1, .tv_nsec = 0};

        reap_installer(&server);
        nanosleep(&delay, NULL);
    }

    MHD_stop_daemon(daemon);
    reap_installer(&server);
    return EXIT_SUCCESS;
}
