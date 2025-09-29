from pyservicemaker import Pipeline

import os
import gi
gi.require_version("Gst", "1.0")
gi.require_version("GstRtspServer", "1.0")
from gi.repository import Gst, GstRtspServer


def start_rtsp_server(rtsp_port=8554, mounts=("/stream0", "/stream1"), udp_ports=(5600, 5602)):
    Gst.init(None)
    server = GstRtspServer.RTSPServer.new()
    server.props.service = str(rtsp_port)
    server.set_address("10.243.223.217")
    mounts_obj = server.get_mount_points()

    def make_factory(udp_port: int):
        factory = GstRtspServer.RTSPMediaFactory.new()
        factory.set_shared(True)
        launch = (
            f"( udpsrc address=127.0.0.1 port={udp_port} caps=\"application/x-rtp,"
            f"media=video,encoding-name=H264,clock-rate=90000,payload=96\" "
            f"! rtph264depay ! h264parse ! rtph264pay name=pay0 pt=96 config-interval=1 )"
        )
        factory.set_launch(launch)
        return factory

    mounts_obj.add_factory(mounts[0], make_factory(udp_ports[0]))
    mounts_obj.add_factory(mounts[1], make_factory(udp_ports[1]))
    server.attach(None)
    print(f"RTSP server listening on rtsp://10.243.223.217:{rtsp_port}{mounts[0]} and {mounts[1]}")
    return server


def main():
    uri0 = os.environ.get(
        "URI0",
        "file:///opt/nvidia/deepstream/deepstream/samples/streams/sample_1080p_h264.mp4",
    )
    uri1 = os.environ.get(
        "URI1",
        "file:///opt/nvidia/deepstream/deepstream/samples/streams/sample_1080p_h265.mp4",
    )

    server = start_rtsp_server(rtsp_port=int(os.environ.get("RTSP_PORT", "8554")))

    pipeline = Pipeline("two_in_two_out")

    pipeline.add(
        "nvmultiurisrcbin",
        "srcs",
        {
            "uri-list": ",".join([uri0, uri1]),
            "sensor-id-list": "0,1",
            "sensor-name-list": "cam0,cam1",
            "live-source": True,
            "file-loop": True,
            "max-batch-size": 2,
            "width": 1280,
            "height": 720,
            "batched-push-timeout": 33000,
        },
    )

    pgie_config = os.environ.get(
        "PGIE_CONFIG",
        "/opt/nvidia/deepstream/deepstream-8.0/samples/configs/deepstream-app/config_infer_primary.txt",
    )
    pipeline.add("nvinfer", "pgie", {"config-file-path": pgie_config})

    pipeline.add("nvosdbin", "osd")
    pipeline.add("nvstreamdemux", "demux")

    # Per-stream branches: demux -> caps -> queue -> nvvideoconvert -> NV12 caps -> encoder -> parse -> pay -> UDP
    pipeline.add("capsfilter", "dcaps0", {"caps": "video/x-raw(memory:NVMM)"})
    pipeline.add("queue", "q0")
    pipeline.add("nvvideoconvert", "conv0")
    pipeline.add("capsfilter", "caps0", {"caps": "video/x-raw(memory:NVMM), format=NV12"})
    pipeline.add("nvv4l2h264enc", "enc0", {"bitrate": 4000000, "insert-sps-pps": 1})
    pipeline.add("h264parse", "h264parse0")
    pipeline.add("rtph264pay", "pay0", {"pt": 96})
    pipeline.add("udpsink", "udp0", {"host": "127.0.0.1", "port": 5600, "async": False, "sync": False})

    pipeline.add("capsfilter", "dcaps1", {"caps": "video/x-raw(memory:NVMM)"})
    pipeline.add("queue", "q1")
    pipeline.add("nvvideoconvert", "conv1")
    pipeline.add("capsfilter", "caps1", {"caps": "video/x-raw(memory:NVMM), format=NV12"})
    pipeline.add("nvv4l2h264enc", "enc1", {"bitrate": 4000000, "insert-sps-pps": 1})
    pipeline.add("h264parse", "h264parse1")
    pipeline.add("rtph264pay", "pay1", {"pt": 96})
    pipeline.add("udpsink", "udp1", {"host": "127.0.0.1", "port": 5602, "async": False, "sync": False})

    # Main chain (nvmultiurisrcbin already does decode + mux internally)
    pipeline.link("srcs", "pgie", "osd", "demux")

    # Demux to branches using pad hints
    pipeline.link(("demux", "dcaps0"), ("src_%u", ""))
    pipeline.link("dcaps0", "q0", "conv0", "caps0", "enc0", "h264parse0", "pay0", "udp0")

    pipeline.link(("demux", "dcaps1"), ("src_%u", ""))
    pipeline.link("dcaps1", "q1", "conv1", "caps1", "enc1", "h264parse1", "pay1", "udp1")

    pipeline.start().wait()


if __name__ == "__main__":
    main()