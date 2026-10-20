import time

from app.ringbuffer import AudioChunk, RingBuffer, VideoFrame


def frame(ts: float, payload: bytes = b"\xff\xd8\xff\xd9") -> VideoFrame:
    return VideoFrame(ts=ts, device_ts=int(ts * 1e6), jpeg=payload)


def test_write_and_window_slice():
    rb = RingBuffer(seconds=40)
    base = time.time()
    for i in range(10):
        rb.add_frame(frame(base + i))
    for i in range(5):
        rb.add_audio(AudioChunk(ts=base + i * 0.5, pcm=b"\x00" * 100))

    frames = rb.frames_between(base + 2, base + 5)
    assert [f.ts for f in frames] == [base + i for i in range(2, 6)]

    audio = rb.audio_between(base + 1.0, base + 1.5)
    assert len(audio) == 2  # ts = 1.0 与 1.5（边界包含）
    assert rb.latest_frame().ts == base + 9


def test_pruning_beyond_capacity():
    rb = RingBuffer(seconds=10)
    base = time.time()
    for i in range(20):
        rb.add_frame(frame(base + i))
        rb.add_audio(AudioChunk(ts=base + i, pcm=b"\x00" * 10))
    # 容量 10s：最早一帧 ts=base 应在 ts=base+19 写入时被裁剪（cutoff=base+9）
    assert rb.frames_between(0, base + 100)[0].ts >= base + 9
    assert rb.audio_between(0, base + 100)[0].ts >= base + 9


def test_frames_since():
    rb = RingBuffer(seconds=40)
    base = time.time()
    for i in range(5):
        rb.add_frame(frame(base + i))
    assert len(rb.frames_since(base + 2)) == 2  # base+3, base+4（严格大于）
    assert len(rb.frames_since(base + 99)) == 0
