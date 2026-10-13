#pragma once
#include <obs-module.h>
#include <obs-frontend-api.h>

#ifdef __cplusplus
extern "C" {
#endif

bool obs_module_load(void);
void obs_module_unload(void);

#ifdef __cplusplus
}
#endif
