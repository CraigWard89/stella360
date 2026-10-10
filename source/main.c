/**
 * @file main.c
 * @brief Stella 360 - Native Atari 2600 Emulator for Xbox 360 (Libxenon).
 *
 * This application provides a standalone, native Xbox 360 execution harness for
 * the Stella Atari 2600 core. It boots directly from USB or HDD without requiring
 * Original Xbox backwards compatibility (HDDX: Partition 2) or Microsoft XDK runtime.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

/* Libxenon Hardware Subsystems */
#include <xenos/xe.h>
#include <xenos/xenos.h>
#include <xenos/edram.h>
#include <console/console.h>
#include <xenon_soc/xenon_power.h>
#include <xenon_sound/sound.h>
#include <usb/usbmain.h>
#include <diskio/ata.h>
#include <input/input.h>
#include <libfat/fat.h>
#include <ppc/timebase.h>

/* Libretro Core Interface */
#include <libretro.h>

#define TEX_WIDTH  320
#define TEX_HEIGHT 256
#define AUDIO_OUT_RATE 48000

/* Vertex format for Xenos Textured Quad */
typedef struct VertexFormat
{
    float x, y, z, w;
    uint32_t color;
    float u, v;
} VertexFormat;

/* Pixel Shader bytecode (Simple 2D texture sampling) */
static const uint32_t g_xps_PS[] =
{
    0x102a1100, 0x000000b4, 0x0000003c, 0x00000000, 0x00000024, 0x00000000,
    0x0000008c, 0x00000000, 0x00000000, 0x00000064, 0x0000001c, 0x00000057,
    0xffff0300, 0x00000001, 0x0000001c, 0x00000000, 0x00000050, 0x00000030,
    0x00030000, 0x00010000, 0x00000040, 0x00000000, 0x54657874, 0x75726553,
    0x616d706c, 0x657200ab, 0x0004000c, 0x00010001, 0x00010000, 0x00000000,
    0x70735f33, 0x5f300032, 0x2e302e32, 0x30333533, 0x2e3000ab, 0x00000000,
    0x0000003c, 0x10000100, 0x00000008, 0x00000000, 0x00001842, 0x00010003,
    0x00000001, 0x00003050, 0x0000f1a0, 0x00011002, 0x00001200, 0xc4000000,
    0x00001003, 0x00002200, 0x00000000, 0x10081001, 0x1f1ff688, 0x00004000,
    0xc80f8000, 0x00000000, 0xe2010100, 0x00000000, 0x00000000, 0x00000000
};

/* Vertex Shader bytecode (Orthographic transformation) */
static const uint32_t g_xvs_VS[] =
{
    0x102a1101, 0x0000009c, 0x00000078, 0x00000000, 0x00000024, 0x00000000,
    0x00000058, 0x00000000, 0x00000000, 0x00000030, 0x0000001c, 0x00000023,
    0xfffe0300, 0x00000000, 0x00000000, 0x00000000, 0x0000001c, 0x76735f33,
    0x5f300032, 0x2e302e32, 0x30333533, 0x2e3000ab, 0x00000000, 0x00000078,
    0x00110002, 0x00000000, 0x00000000, 0x00001842, 0x00000001, 0x00000003,
    0x00000002, 0x00000290, 0x00100003, 0x0000a004, 0x00305005, 0x00003050,
    0x0001f1a0, 0x00001007, 0x00001008, 0x70153003, 0x00001200, 0xc2000000,
    0x00001006, 0x00001200, 0xc4000000, 0x00002007, 0x00002200, 0x00000000,
    0x05f82000, 0x00000688, 0x00000000, 0x05f81000, 0x00000688, 0x00000000,
    0x05f80000, 0x00000fc8, 0x00000000, 0xc80f803e, 0x00000000, 0xe2020200,
    0xc8038000, 0x00b0b000, 0xe2000000, 0xc80f8001, 0x00000000, 0xe2010100,
    0x00000000, 0x00000000, 0x00000000
};

/* Global Graphics and Emulator State */
static struct XenosDevice g_xe_device;
static struct XenosSurface *g_xe_texture = NULL;
static struct XenosVertexBuffer *g_xe_vb = NULL;
static struct XenosShader *g_xe_ps = NULL;
static struct XenosShader *g_xe_vs = NULL;

static enum retro_pixel_format g_pixel_format = RETRO_PIXEL_FORMAT_RGB565;
static struct retro_system_av_info g_av_info;
static bool g_game_loaded = false;
static bool g_running = true;

/* Controller state cache */
static struct controller_data_s g_pad;
static uint16_t g_retro_pad_state = 0;

/* Audio Resampler Buffer */
#define AUDIO_BUF_SIZE 4096
static int16_t g_audio_resample_buf[AUDIO_BUF_SIZE * 2];

/**
 * @brief Libretro Environment Callback.
 */
static bool cb_environment(unsigned cmd, void *data)
{
    switch (cmd)
    {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        {
            const enum retro_pixel_format *fmt = (const enum retro_pixel_format *)data;
            if (*fmt == RETRO_PIXEL_FORMAT_RGB565 || *fmt == RETRO_PIXEL_FORMAT_0RGB1555)
            {
                g_pixel_format = *fmt;
                return true;
            }
            return false;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE:
        case RETRO_ENVIRONMENT_SET_VARIABLES:
            return false;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool *)data = true;
            return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            return false;
        default:
            return false;
    }
}

/**
 * @brief Libretro Video Refresh Callback.
 * Updates the Xenos texture with the newly rendered frame from Stella.
 */
static void cb_video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
    if (!data || width == 0 || height == 0 || !g_xe_texture)
        return;

    /* Lock Xenos texture surface for memory write */
    uint16_t *tex_dst = (uint16_t *)Xe_Surface_LockRect(&g_xe_device, g_xe_texture, 0, 0, width, height, XE_LOCK_WRITE);
    if (!tex_dst)
        return;

    const uint8_t *src_bytes = (const uint8_t *)data;
    unsigned tex_stride_pixels = g_xe_texture->wpitch / sizeof(uint16_t);

    for (unsigned y = 0; y < height; y++)
    {
        const uint16_t *src_line = (const uint16_t *)(src_bytes + y * pitch);
        uint16_t *dst_line = tex_dst + (y * tex_stride_pixels);
        memcpy(dst_line, src_line, width * sizeof(uint16_t));
    }

    Xe_Surface_Unlock(&g_xe_device, g_xe_texture);
}

/**
 * @brief Libretro Audio Sample Callback (Mono/Single Frame).
 */
static void cb_audio_sample(int16_t left, int16_t right)
{
    int16_t frame[2] = { left, right };
    xenon_sound_submit(frame, sizeof(frame));
}

/**
 * @brief Libretro Audio Batch Callback with Linear Resampling to 48kHz.
 */
static size_t cb_audio_sample_batch(const int16_t *data, size_t frames)
{
    if (!data || frames == 0)
        return 0;

    double in_rate = g_av_info.timing.sample_rate > 0 ? g_av_info.timing.sample_rate : 31400.0;
    double ratio = (double)AUDIO_OUT_RATE / in_rate;
    size_t out_frames = (size_t)(frames * ratio);

    if (out_frames > AUDIO_BUF_SIZE)
        out_frames = AUDIO_BUF_SIZE;

    for (size_t i = 0; i < out_frames; i++)
    {
        size_t src_idx = (size_t)(i / ratio);
        if (src_idx >= frames)
            src_idx = frames - 1;

        g_audio_resample_buf[i * 2]     = data[src_idx * 2];
        g_audio_resample_buf[i * 2 + 1] = data[src_idx * 2 + 1];
    }

    xenon_sound_submit(g_audio_resample_buf, out_frames * 4);
    return frames;
}

/**
 * @brief Libretro Input Poll Callback.
 * Polls USB hardware and maps the Xbox 360 controller to Atari 2600 switches and joystick.
 */
static void cb_input_poll(void)
{
    usb_do_poll();
    get_controller_data(&g_pad, 0);

    g_retro_pad_state = 0;

    /* Directional mapping: D-Pad & Left Analog Stick */
    if (g_pad.up || g_pad.s1_y > 16000)
        g_retro_pad_state |= (1 << RETRO_DEVICE_ID_JOYPAD_UP);
    if (g_pad.down || g_pad.s1_y < -16000)
        g_retro_pad_state |= (1 << RETRO_DEVICE_ID_JOYPAD_DOWN);
    if (g_pad.left || g_pad.s1_x < -16000)
        g_retro_pad_state |= (1 << RETRO_DEVICE_ID_JOYPAD_LEFT);
    if (g_pad.right || g_pad.s1_x > 16000)
        g_retro_pad_state |= (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT);

    /* Atari Action Buttons (A, X, or Right Trigger = Fire) */
    if (g_pad.a || g_pad.x || g_pad.rt > 30)
        g_retro_pad_state |= (1 << RETRO_DEVICE_ID_JOYPAD_B);

    /* Atari Console Switches */
    if (g_pad.back)
        g_retro_pad_state |= (1 << RETRO_DEVICE_ID_JOYPAD_SELECT); /* Select Switch */
    if (g_pad.start)
        g_retro_pad_state |= (1 << RETRO_DEVICE_ID_JOYPAD_START);  /* Reset Switch (Starts Game) */
    if (g_pad.lb)
        g_retro_pad_state |= (1 << RETRO_DEVICE_ID_JOYPAD_L);      /* P1 Difficulty */
    if (g_pad.rb)
        g_retro_pad_state |= (1 << RETRO_DEVICE_ID_JOYPAD_R);      /* P2 Difficulty */
    if (g_pad.lt > 30)
        g_retro_pad_state |= (1 << RETRO_DEVICE_ID_JOYPAD_L2);     /* Color / B&W */

    /* Combo Exit: Back + Start */
    if (g_pad.back && g_pad.start)
    {
        printf("Exit requested by user (Back + Start).\n");
        g_running = false;
    }
}

/**
 * @brief Libretro Input State Callback.
 */
static int16_t cb_input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)device;
    (void)index;

    if (port == 0 && id < 16)
    {
        return (g_retro_pad_state & (1 << id)) ? 1 : 0;
    }
    return 0;
}

/**
 * @brief Initializes Xenos 3D hardware for 2D orthographic rendering.
 */
static void init_xenos_video(void)
{
    static const struct XenosVBFFormat vbf =
    {
        3,
        {
            { XE_USAGE_POSITION, 0, XE_TYPE_FLOAT4 },
            { XE_USAGE_COLOR,    0, XE_TYPE_UBYTE4 },
            { XE_USAGE_TEXCOORD, 0, XE_TYPE_FLOAT2 },
        }
    };

    Xe_Init(&g_xe_device);
    Xe_SetRenderTarget(&g_xe_device, Xe_GetFramebufferSurface(&g_xe_device));
    edram_init(&g_xe_device);

    /* Load and instantiate shaders */
    g_xe_ps = Xe_LoadShaderFromMemory(&g_xe_device, (void *)g_xps_PS);
    Xe_InstantiateShader(&g_xe_device, g_xe_ps, 0);

    g_xe_vs = Xe_LoadShaderFromMemory(&g_xe_device, (void *)g_xvs_VS);
    Xe_InstantiateShader(&g_xe_device, g_xe_vs, 0);
    Xe_ShaderApplyVFetchPatches(&g_xe_device, g_xe_vs, 0, &vbf);

    /* Create texture for 16-bit big-endian video */
    g_xe_texture = Xe_CreateTexture(&g_xe_device, TEX_WIDTH, TEX_HEIGHT, 1, XE_FMT_565 | XE_FMT_16BE, 0);
    if (g_xe_texture)
        g_xe_texture->use_filtering = 1;

    /* Build fullscreen textured quad */
    g_xe_vb = Xe_CreateVertexBuffer(&g_xe_device, 4 * sizeof(VertexFormat));
    VertexFormat *rect = (VertexFormat *)Xe_VB_Lock(&g_xe_device, g_xe_vb, 0, 4 * sizeof(VertexFormat), XE_LOCK_WRITE);
    if (rect)
    {
        /* Top-Left */
        rect[0].x = -1.0f; rect[0].y =  1.0f; rect[0].z = 0.0f; rect[0].w = 1.0f;
        rect[0].u = 0.0f;  rect[0].v = 0.0f; rect[0].color = 0xFFFFFFFF;

        /* Bottom-Left */
        rect[1].x = -1.0f; rect[1].y = -1.0f; rect[1].z = 0.0f; rect[1].w = 1.0f;
        rect[1].u = 0.0f;  rect[1].v = 1.0f; rect[1].color = 0xFFFFFFFF;

        /* Top-Right */
        rect[2].x =  1.0f; rect[2].y =  1.0f; rect[2].z = 0.0f; rect[2].w = 1.0f;
        rect[2].u = 1.0f;  rect[2].v = 0.0f; rect[2].color = 0xFFFFFFFF;

        /* Bottom-Right */
        rect[3].x =  1.0f; rect[3].y = -1.0f; rect[3].z = 0.0f; rect[3].w = 1.0f;
        rect[3].u = 1.0f;  rect[3].v = 1.0f; rect[3].color = 0xFFFFFFFF;

        Xe_VB_Unlock(&g_xe_device, g_xe_vb);
    }
}

/**
 * @brief Renders the current frame to the screen via Xenos.
 */
static void render_frame(void)
{
    Xe_SetClearColor(&g_xe_device, 0x00000000);
    Xe_SetRenderTarget(&g_xe_device, Xe_GetFramebufferSurface(&g_xe_device));
    Xe_SetShader(&g_xe_device, SHADER_TYPE_PIXEL, g_xe_ps, 0);
    Xe_SetShader(&g_xe_device, SHADER_TYPE_VERTEX, g_xe_vs, 0);
    Xe_SetTexture(&g_xe_device, 0, g_xe_texture);
    Xe_SetStreamSource(&g_xe_device, 0, g_xe_vb, 0, sizeof(VertexFormat));
    Xe_DrawPrimitive(&g_xe_device, XE_PRIMTYPE_RECTLIST, 0, 1);
    Xe_Execute(&g_xe_device);
    Xe_Sync(&g_xe_device);
}

/**
 * @brief Searches for SwordQuest or any Atari 2600 ROM on storage devices.
 */
static char *find_target_rom(void)
{
    static const char *candidate_paths[] = {
        "uda:/SwordQuest - EarthWorld (USA).a26",
        "uda:/SwordQuest - EarthWorld.a26",
        "uda:/Emulators/Atari2600/a26roms/SwordQuest - EarthWorld (USA).a26",
        "uda:/Emulators/Stella360/SwordQuest - EarthWorld (USA).a26",
        "uda:/Emulators/Stella360/roms/SwordQuest - EarthWorld (USA).a26",
        "uda:/roms/SwordQuest - EarthWorld (USA).a26",
        "sda:/Emulators/Atari2600/a26roms/SwordQuest - EarthWorld (USA).a26",
        NULL
    };

    for (int i = 0; candidate_paths[i] != NULL; i++)
    {
        FILE *f = fopen(candidate_paths[i], "rb");
        if (f)
        {
            fclose(f);
            return (char *)candidate_paths[i];
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    /* Initialize base video and console for diagnostic output */
    xenos_init(VIDEO_MODE_AUTO);
    console_init();
    xenon_make_it_faster(XENON_SPEED_FULL);

    printf("\n======================================================\n");
    printf("   Stella 360 - Native Atari 2600 for Xbox 360\n");
    printf("   PowerPC Xenon Architecture Port by CraigWard89\n");
    printf("======================================================\n\n");

    /* Initialize USB, Storage and Audio Subsystems */
    printf("[Init] Initializing USB subsystem...\n");
    usb_init();
    usb_do_poll();

    printf("[Init] Initializing ATA / Storage subsystem...\n");
    xenon_ata_init();

    printf("[Init] Mounting FAT filesystems...\n");
    fatInitDefault();

    printf("[Init] Initializing Xenon 48kHz audio...\n");
    xenon_sound_init();

    /* Find target game ROM */
    const char *rom_path = NULL;
    if (argc > 1 && argv[1])
    {
        rom_path = argv[1];
    }
    else
    {
        rom_path = find_target_rom();
    }

    if (!rom_path)
    {
        printf("[Error] Could not locate SwordQuest ROM on USB (uda:/) or HDD (sda:/)!\n");
        printf("Please ensure 'SwordQuest - EarthWorld (USA).a26' is placed on your USB drive.\n");
        printf("Press any controller button or power off.\n");
        while (1) { usb_do_poll(); }
    }

    printf("[ROM] Found target cartridge: %s\n", rom_path);

    FILE *f = fopen(rom_path, "rb");
    if (!f)
    {
        printf("[Error] Failed to open ROM file: %s\n", rom_path);
        while (1) { usb_do_poll(); }
    }

    fseek(f, 0, SEEK_END);
    long rom_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    void *rom_data = malloc(rom_size);
    if (!rom_data)
    {
        printf("[Error] Out of memory allocating %ld bytes for ROM.\n", rom_size);
        fclose(f);
        while (1) { usb_do_poll(); }
    }

    fread(rom_data, 1, rom_size, f);
    fclose(f);
    printf("[ROM] Loaded %ld bytes into memory.\n", rom_size);

    /* Initialize Libretro Core */
    printf("[Core] Setting Libretro callbacks...\n");
    retro_set_environment(cb_environment);
    retro_set_video_refresh(cb_video_refresh);
    retro_set_audio_sample(cb_audio_sample);
    retro_set_audio_sample_batch(cb_audio_sample_batch);
    retro_set_input_poll(cb_input_poll);
    retro_set_input_state(cb_input_state);

    printf("[Core] Initializing Stella Atari 2600 engine...\n");
    retro_init();

    struct retro_game_info game_info;
    memset(&game_info, 0, sizeof(game_info));
    game_info.path = rom_path;
    game_info.data = rom_data;
    game_info.size = rom_size;

    if (!retro_load_game(&game_info))
    {
        printf("[Error] Stella core failed to load game cartridge!\n");
        retro_deinit();
        free(rom_data);
        while (1) { usb_do_poll(); }
    }

    g_game_loaded = true;
    retro_get_system_av_info(&g_av_info);
    printf("[Core] Cartridge verified! Resolution: %ux%u @ %.2f fps\n",
           g_av_info.geometry.base_width, g_av_info.geometry.base_height, g_av_info.timing.fps);

    /* Initialize Xenos 3D acceleration for fullscreen scaling */
    printf("[Video] Initializing Xenos graphics pipeline...\n");
    init_xenos_video();

    printf("[Run] Starting emulation loop. Press START to reset/start game.\n");

    /* 60Hz Main Emulation Loop (50MHz timebase on Xenon) */
    const uint64_t ticks_per_frame = 50000000 / 60;
    uint64_t next_frame_time = mftb() + ticks_per_frame;

    while (g_running)
    {
        /* Run one Atari 2600 frame */
        retro_run();

        /* Present frame via Xenos */
        render_frame();

        /* Throttle to 60 FPS */
        while (mftb() < next_frame_time) { }
        next_frame_time += ticks_per_frame;
    }

    /* Cleanup */
    printf("[Shutdown] Unloading game and freeing resources...\n");
    if (g_game_loaded)
    {
        retro_unload_game();
    }
    retro_deinit();
    free(rom_data);

    printf("[Shutdown] Emulation ended. Returning to dashboard.\n");
    return 0;
}
