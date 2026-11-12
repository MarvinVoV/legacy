from scripts.voice_health_observer import heartbeat_sample


def test_heartbeat_observer_does_not_copy_arbitrary_device_data():
    sample = heartbeat_sample(
        b'{"heap":50000,"heap_min":35000,"uptime_s":600,'
        b'"esp_build":"build1","private_extra":"never copy"}'
    )
    assert sample == {"heap": 50000, "heap_min": 35000, "uptime_s": 600, "esp_build": "build1"}
    for payload in (
        b"oops",
        b"[]",
        b'{"heap":true,"heap_min":5,"uptime_s":10}',
        b'{"heap":1,"heap_min":5,"uptime_s":-1}',
    ):
        assert heartbeat_sample(payload) is None
