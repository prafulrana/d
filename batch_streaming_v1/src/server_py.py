import gi
gi.require_version('Gst', '1.0')
gi.require_version('GstRtspServer', '1.0')
from gi.repository import Gst, GstRtspServer, GLib
import os
import requests
from flask import Flask, jsonify
from threading import Thread

app = Flask(__name__)

# Globals
pipeline = None
demux = None
server = None
current_streams = 0
max_streams = 64

# Env vars
RTSP_PORT = int(os.environ.get('RTSP_PORT', 8554))
BASE_UDP_PORT = int(os.environ.get('BASE_UDP_PORT', 5000))
PUBLIC_HOST = os.environ.get('PUBLIC_HOST', '127.0.0.1')
SAMPLE_URI = os.environ.get('SAMPLE_URI', 'file:///opt/nvidia/deepstream/deepstream/samples/streams/sample_1080p_h264.mp4')
USE_OSD = int(os.environ.get('USE_OSD', 1)) == 1
# DeepStream REST default ports seen in samples
REST_PORT = int(os.environ.get('REST_PORT', 9000))
REST_PORTS = [str(REST_PORT), '9010', '9000']


def bus_call(bus, message, loop):
    msg_type = message.type
    if msg_type == Gst.MessageType.EOS:
        print('End of stream')
        loop.quit()
    elif msg_type == Gst.MessageType.ERROR:
        err, debug = message.parse_error()
        print(f'Error: {err.message}, Debug: {debug}')
        loop.quit()
    return True


def create_pipeline():
    global pipeline, demux
    Gst.init(None)
    pipeline = Gst.Pipeline.new('batch-streaming-pipeline')

    # nvmultiurisrcbin (dynamic sources, start empty)
    multiuri = Gst.ElementFactory.make('nvmultiurisrcbin', 'multiuri')
    multiuri.set_property('max-batch-size', max_streams)
    # Start with no sources and enable internal REST API on REST_PORT
    multiuri.set_property('uri-list', '')
    multiuri.set_property('port', REST_PORT)
    multiuri.set_property('width', 1280)
    multiuri.set_property('height', 720)
    multiuri.set_property('batched-push-timeout', 100000)
    multiuri.set_property('file-loop', True)
    multiuri.set_property('sync-inputs', False)
    multiuri.set_property('attach-sys-ts', True)
    multiuri.set_property('drop-on-latency', False)

    # nvinfer (from pgie.txt)
    pgie = Gst.ElementFactory.make('nvinfer', 'pgie')
    pgie.set_property('config-file-path', '/opt/nvidia/deepstream/deepstream-8.0/pgie.txt')

    # nvstreamdemux
    demux = Gst.ElementFactory.make('nvstreamdemux', 'demux')

    pipeline.add(multiuri)
    pipeline.add(pgie)
    pipeline.add(demux)

    if not multiuri.link(pgie):
        raise RuntimeError('Failed to link multiuri->pgie')
    if not pgie.link(demux):
        raise RuntimeError('Failed to link pgie->demux')

    return pipeline


def _post_add_source(index):
    payload = {
        'key': 'sensor',
        'value': {
            'camera_id': f's{index}',
            'camera_name': f'Stream {index}',
            'camera_url': SAMPLE_URI,
            'change': 'camera_add',
            'metadata': {
                'resolution': '1280x720',
                'codec': 'h264',
                'framerate': 30
            }
        },
        'headers': {
            'source': 'vst'
        }
    }
    for port in REST_PORTS:
        port = str(port).strip()
        if not port:
            continue
        try:
            url = f'http://127.0.0.1:{port}/api/v1/stream/add'
            r = requests.post(url, json=payload, timeout=3)
            if r.status_code == 200:
                print(f'Added stream {index} via REST @ {port}')
                return True
            else:
                print(f'REST add failed @ {port}: {r.status_code} {r.text}')
        except Exception as e:
            print(f'REST connect failed @ {port}: {e}')
    return False


def add_stream(index):
    global pipeline, demux
    src_pad = demux.get_request_pad(f'src_{index}')
    if not src_pad:
        raise RuntimeError(f'Failed to request demux pad src_{index}')

    # Per‑stream branch (post‑demux)
    branch_desc = (
        'queue leaky=2 max-size-time=200000000 ! '
        'nvvideoconvert ! video/x-raw(memory:NVMM),format=RGBA ! '
        + ('nvdsosd ! ' if USE_OSD else '') +
        'nvvideoconvert ! video/x-raw(memory:NVMM),format=NV12,framerate=30/1 ! '
        'nvv4l2h264enc insert-sps-pps=1 idrinterval=30 iframeinterval=30 bitrate=3000000 ! '
        'h264parse ! rtph264pay pt=96 config-interval=1 ! '
        f'udpsink host=127.0.0.1 port={BASE_UDP_PORT + index} sync=false'
    )

    branch_bin = Gst.parse_bin_from_description(branch_desc, True)
    pipeline.add(branch_bin)
    if src_pad.link(branch_bin.get_static_pad('sink')) != Gst.PadLinkReturn.OK:
        raise RuntimeError('Failed to link demux->branch')
    branch_bin.sync_state_with_parent()

    # Mount RTSP factory (UDP wrap)
    factory = GstRtspServer.RTSPMediaFactory.new()
    factory.set_launch(
        f'( udpsrc port={BASE_UDP_PORT + index} buffer-size=524288 '
        'caps="application/x-rtp, media=(string)video, clock-rate=(int)90000, '
        'encoding-name=(string)H264, payload=(int)96" )'
    )
    factory.set_shared(True)
    server.get_mount_points().add_factory(f'/s{index}', factory)
    print(f'Mounted RTSP at /s{index} (UDP port {BASE_UDP_PORT + index})')

    # Add source via nvmultiurisrcbin REST API (best effort)
    _post_add_source(index)


def setup_rtsp_server():
    global server
    server = GstRtspServer.RTSPServer.new()
    server.set_service(str(RTSP_PORT))

    # Add /test endpoint (synthetic source for sanity)
    test_factory = GstRtspServer.RTSPMediaFactory.new()
    test_factory.set_launch(
        '( videotestsrc is-live=true pattern=smpte ! '
        'videoconvert ! jpegenc quality=85 ! rtpjpegpay name=pay0 pt=26 )'
    )
    server.get_mount_points().add_factory('/test', test_factory)

    server.attach(None)
    print(f'RTSP server listening on port {RTSP_PORT}')


@app.route('/add_demo_stream', methods=['GET'])
def add_demo_stream():
    global current_streams
    if current_streams >= max_streams:
        return jsonify({'error': 'capacity_exceeded', 'max': max_streams}), 429
    try:
        add_stream(current_streams)
        path = f'/s{current_streams}'
        url = f'rtsp://{PUBLIC_HOST}:{RTSP_PORT}{path}'
        current_streams += 1
        return jsonify({'path': path, 'url': url})
    except Exception as e:
        return jsonify({'error': str(e)}), 500


if __name__ == '__main__':
    pipeline = create_pipeline()
    bus = pipeline.get_bus()
    bus.add_signal_watch()
    loop = GLib.MainLoop.new(None, False)
    bus.connect('message', bus_call, loop)

    setup_rtsp_server()

    pipeline.set_state(Gst.State.PLAYING)
    print('Pipeline started')

    # Run Flask API on port 8080
    flask_thread = Thread(target=app.run, kwargs={'host': '0.0.0.0', 'port': 8080, 'debug': False, 'use_reloader': False})
    flask_thread.daemon = True
    flask_thread.start()

    try:
        loop.run()
    except KeyboardInterrupt:
        pass
    finally:
        pipeline.set_state(Gst.State.NULL)
        print('Pipeline stopped')
