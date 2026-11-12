"""Compile the actual production feed_binary function with minimal host shims.

Requires Python 3 and clang. Uses a temporary directory; no firmware/hardware.
The assertion confirms the reviewed defect, not desired fixed behavior.
"""

import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
SOURCE = ROOT / "esp32_project/components/voice_service/src/voice_transport.c"

PRELUDE = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_LOGW(...) ((void)0)
static const char *esp_err_to_name(int x) { (void)x; return "err"; }
typedef struct {
 size_t speak_remaining; int speak_rate,speak_channels;
 volatile bool speaking,local_barge; uint8_t speak_carry; bool speak_have_carry;
 int16_t speak_pcm[1025];
 esp_err_t (*audio_cb)(const int16_t*,size_t,int,int,void*); void *audio_arg;
} voice_transport_t;
static int received;
static esp_err_t output(const int16_t *pcm,size_t n,int rate,int ch,void *arg) {
 (void)pcm;(void)rate;(void)ch;(void)arg; received+=(int)n; return ESP_OK;
}
"""

MAIN = r"""
int main(void) {
 voice_transport_t t={.speak_remaining=4,.speaking=true,.speak_rate=16000,
                       .speak_channels=1,.audio_cb=output};
 const char bytes[4]={1,0,2,0};
 feed_binary(&t,bytes,1);feed_binary(&t,bytes+1,3);
 printf("4 bytes split 1+3: emitted=%d samples, remaining=%zu, speaking=%d\n",
        received,t.speak_remaining,t.speaking);
 assert(received==2 && t.speak_remaining==1 && t.speaking);
}
"""


def main():
    source = SOURCE.read_text()
    body = source[source.index("static void feed_binary("):source.index("\nstatic void on_event(")]
    with tempfile.TemporaryDirectory(prefix="cmcar-voice-carry-") as directory:
        file = Path(directory) / "reproduce.c"
        binary = Path(directory) / "reproduce"
        file.write_text(PRELUDE + body + MAIN)
        subprocess.run(["clang", "-std=c11", "-fsanitize=address,undefined",
                        str(file), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
