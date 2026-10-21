# Ratchet & Clank: shadow flicker and the motion blur

Shadows flicker while the camera moves. The game's motion blur makes it much worse in some places,
so it is off by default (`effect_motion_blur=0` in `bbport.ini`, the launcher's Motion blur
switch), but the flicker is also seen with it off (on Veldin, for example). This page is what is
known so far, for anyone who wants to dig in.

## What you see

With motion blur on, shadows come and go between frames while the camera turns. In the sunlit
garage, the shadow of the roof on the wall is in some frames and missing from the next. It happens
at 30 and 60 FPS, with DLSS or the game's own TAA, and in builds back to mid October. With motion
blur off the shadows in that scene are steady, but shadows still flicker elsewhere.

## The game's motion blur

One frame, in order (the port's shader hashes):

| Pass | Shader | Reads | Writes |
|---|---|---|---|
| Tile velocity | cs `233341ad` | velocity 1920x1080 RG16F | 480x270, 120x68 tiles |
| Neighbour max | cs `bfbd37b0` | 120x68 tiles | 120x68 |
| Blur | cs `2655cbd4` | velocity, linear depth, scene (B10G11R11) | half-res velocity, half-res colour (RGBA16F) |
| Temporal | cs `1fbf981c` | half-res colour, velocity, history | blurred colour A (960x540 B10G11R11) |
| Final | cs `0354eebe` | tiles, half-res velocity, A | mask (R8), blurred colour B |
| Blend | ps `e9bef76c` | mask, velocity, B | scene (blended) |

A and B swap every frame (the history is the other one). In a fast turn the mask covers about 95%
of the screen, so the blend replaces most of the image with the half-res blurred colour. With
motion blur off the port leaves out only the blend draw (`SkipMotionBlur` in `vk_rasterizer.cpp`).

## What is ruled out

- Frame captures (`BB_CAPTURE_DIR`, `BB_CAPTURE_IMAGES=1`) taken mid-turn: every input and output of
  the chain matches the current frame, and the scene the blur reads has its shadows. A capture
  serializes the GPU work, so the bug needs normal timing.
- The shadow map is identical across consecutive frames.
- Vulkan synchronization validation, per command buffer and submit-time: no hazards on these images
  (only a startup write-after-write on a clear).
- Image refreshes from guest memory: only the game's compute clears of scene-sized targets, none of
  the blur's images.
- Fences written only once the GPU has finished (`BB_HONEST_LABELS=1`): no change.
- Every optimization toggle off (`BB_TOGGLE_FILE`), the upscaler, the frame rate, the light shafts.

## Leads

- The game submits five compute queue jobs a frame. If the blur passes run on a compute queue and
  start before the graphics work that draws the shadows has run, the blur would read a scene
  without them. The next step is to log which queue each blur dispatch comes from.
- The blur's constants (48 bytes a pass, in the per-frame ring) are written by the CPU each frame.
