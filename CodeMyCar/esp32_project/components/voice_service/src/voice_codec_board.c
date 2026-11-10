/* Board codec placeholder for the upcoming microphone/amplifier board.
 *
 * Do not invent a pinout or codec configuration before the hardware and
 * schematic are known. Selecting CMCAR_VOICE_CODEC_BOARD without providing an
 * implementation fails closed and leaves the control bridge running. */
#include "voice_codec.h"

esp_err_t voice_codec_board_init(voice_codec_t **out)
{
    if (out != NULL) *out = NULL;
    return ESP_ERR_NOT_SUPPORTED;
}
