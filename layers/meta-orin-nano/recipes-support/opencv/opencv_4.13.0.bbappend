# This image uses OpenCV's CPU StereoSGBM implementation. Do not build the
# optional CUDA modules or fetch NVIDIA's separate optical-flow SDK archive.
PACKAGECONFIG:remove:pn-opencv = "cuda"
OECMAKE_CUDA_ARCHITECTURES:pn-opencv = "OFF"

SRC_URI:remove:pn-opencv = "https://github.com/NVIDIA/NVIDIAOpticalFlowSDK/archive/${OPTICALFLOW_HASH}.zip;name=opticalflow;unpack=false;downloadfilename=${OPTICALFLOW_MD5}-${OPTICALFLOW_HASH}.zip"
