/*
 * AV encoder surface: AvSendTVEncoderOption settings query and a real
 * AvSetDisplayMode (via nxdk's XVideoSetMode wrapper), which drives the
 * encoder mode-calculation and CRTC programming paths.
 */

#include "../harness.h"
#include <hal/video.h>

#define AV_OPTION_GET_SETTINGS   6
#define AV_OPTION_FLICKER_FILTER 11
#define AV_OPTION_SOFTEN_FILTER  14

static bool t_get_settings(void)
{
    ULONG settings = 0xDEADBEEF;
    AvSendTVEncoderOption(NULL, AV_OPTION_GET_SETTINGS, 0, &settings);
    ASSERT_TRUE(settings != 0xDEADBEEF);

    /* Low byte is the AV pack (0..6), and a region nibble is present. */
    ASSERT_TRUE((settings & 0xFF) <= 6);
    ASSERT_TRUE((settings & 0x0F00) != 0);
    return true;
}

static bool t_filter_options_accepted(void)
{
    /* Parameter-carrying options with no observable result: they must
     * simply return (retail programs the encoder, nxkrnl may ignore). */
    AvSendTVEncoderOption(NULL, AV_OPTION_FLICKER_FILTER, 5, NULL);
    AvSendTVEncoderOption(NULL, AV_OPTION_SOFTEN_FILTER, 0, NULL);
    return true;
}

static bool t_set_display_mode(void)
{
    /* Full mode set: allocates a framebuffer and runs AvSetDisplayMode's
     * encoder + CRTC programming.  640x480x32 exists on every AV pack. */
    ASSERT_TRUE(XVideoSetMode(640, 480, 32, REFRESH_DEFAULT));
    ASSERT_NOT_NULL(XVideoGetFB());
    return true;
}

static const test_entry_t hal_av_entries[] = {
    {"get_settings",            t_get_settings},
    {"filter_options_accepted", t_filter_options_accepted},
    {"set_display_mode",        t_set_display_mode},
};

DEFINE_GROUP(hal_av, "hal/av");
