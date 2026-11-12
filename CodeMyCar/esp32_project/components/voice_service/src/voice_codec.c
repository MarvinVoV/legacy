#include "voice_codec.h"

#include "sdkconfig.h"

esp_err_t voice_codec_init(voice_codec_t **out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    *out = NULL;
#if CONFIG_CMCAR_VOICE_CODEC_BOARD
    return voice_codec_board_init(out);
#else
    return voice_codec_i2s_init(out);
#endif
}

void voice_codec_destroy(voice_codec_t *codec)
{
    if (codec != NULL && codec->ops != NULL && codec->ops->destroy != NULL) {
        codec->ops->destroy(codec);
    }
}

bool voice_codec_has_output(const voice_codec_t *codec)
{
    return codec != NULL && codec->caps.output_channels > 0;
}

esp_err_t voice_codec_read(voice_codec_t *codec, int16_t *dst, size_t frames)
{
    if (codec == NULL || codec->ops == NULL || codec->ops->read == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return codec->ops->read(codec, dst, frames);
}

esp_err_t voice_codec_write(voice_codec_t *codec, const int16_t *src,
                            size_t frames, int sample_rate)
{
    if (!voice_codec_has_output(codec) || codec->ops->write == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return codec->ops->write(codec, src, frames, sample_rate);
}

void voice_codec_set_volume(voice_codec_t *codec, int volume)
{
    if (codec != NULL && codec->ops != NULL && codec->ops->set_volume != NULL) {
        codec->ops->set_volume(codec, volume);
    }
}
