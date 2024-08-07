
#ifndef SYSTEM_H
#define SYSTEM_H

#ifdef	__cplusplus
extern "C" {
#endif
    
#include <stdint.h>
#include <stddef.h>
#include <SDL2/SDL_platform.h>
#include <SDL2/SDL_opengl.h>

#define SYS_LOG_FILENAME            "d_log.txt"

enum debug_view_state_e
{
    no_debug = 0,
    player_anim,
    sector_info,
    room_objects,
    ai_boxes,
    bsp_info,
    model_view,
    debug_states_count
};
    
typedef struct screen_info_s
{
    int16_t     x;
    int16_t     y;
    int16_t     w;  
    int16_t     h;  
    
    float       fps;
    float       fov;
    float       scale_factor;
    uint32_t    debug_view_state : 8;
    uint32_t    fullscreen : 1;
    uint32_t    crosshair : 1;
} screen_info_t, *screen_info_p;

typedef struct file_info_s
{
    char                   *full_name;
    char                   *name;
    uint32_t                is_dir : 1;
    struct file_info_s     *next;
} file_info_t, *file_info_p;


extern screen_info_t screen_info;

void Sys_Init();
void Sys_InitGlobals();
void Sys_Destroy();

void *Sys_GetTempMem(size_t size);
void Sys_ReturnTempMem(size_t size);
void Sys_ResetTempMem();

file_info_p Sys_ListDir(const char *path, const char *wild);
void Sys_ListDirFree(file_info_p list);

void Sys_Strtime(char *buf, size_t buf_size);

void Sys_Init(void);
void Sys_Error(const char *error, ...);
void Sys_Warn(const char *warning, ...);
void Sys_DebugLog(const char *file, const char *fmt, ...);

void Sys_WriteTGAfile(const char *filename, const uint8_t *data, const int width, const int height, int bpp, char invY);
void Sys_TakeScreenShot();

int Sys_FileFound(const char *name, int checkWrite);

#define Sys_LogCurrPlace Sys_DebugLog(SYS_LOG_FILENAME, "\"%s\" str = %d\n", __FILE__, __LINE__);
#define Sys_extError(...) {Sys_LogCurrPlace Sys_Error(__VA_ARGS__);}
#define Sys_extWarn(...) {Sys_LogCurrPlace Sys_Warn(__VA_ARGS__);}

#ifdef	__cplusplus
}
#endif

#endif
