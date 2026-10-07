// ui.h — ImGui + SDL2 frontend. Three tabs: Store / Downloads / Settings.
#pragma once

#include <SDL2/SDL.h>

namespace fs_ui {

struct UiCtx; // opaque

UiCtx* init(SDL_Window* window, SDL_Renderer* renderer);
void   shutdown(UiCtx* ctx);

// Called every frame: handles events, builds the UI, renders. Returns false
// when user requested exit.
bool   frame(UiCtx* ctx, SDL_Window* window, SDL_Renderer* renderer);

} // namespace fs_ui
