# Pixelica

<!-- impeccable:product-schema 1 -->

## Product Purpose

Portable C game libraries and a local editor that helps developers build Pixelica games.
The first editor version supports 2D object placement, properties, project save/open and play/stop.
Its collision extension adds optional solid/trigger regions, static/dynamic bodies, gravity and jumping.
The user explicitly requested shared structure that can accommodate a future 3D editor.

## Users

Developers making 2D, 2.5D and 3D games with Pixelica. Ease of use is a stated priority.

## Platform

Existing engine: portable C11, PC and ESP32-P4 port. No physical board is available for verification.
Editor implementation assumption for this requested fast prototype: local desktop, Windows first.

## Stack

Engine: C11 and CMake. Editor implementation choice: Python/Tk with Pillow and a ctypes bridge
to the existing 2D renderer. No web service or additional C rendering framework.

## Capabilities and Constraints

Game SDK packages remain independent of the editor. The editor must display real Pixelica output,
preserve edit data when play stops, validate project files and protect existing saves on write failure.
3D editing, standalone game export and visual scripting are outside this first editor request.

## Brand Commitments

The project name is Pixelica. Korean is used for the initial editing interface.

## Product Principles

- Make common game development actions easy to discover.
- Reuse existing rendering and asset capabilities.
- Keep project data separate from UI and transient execution state.
- Prefer a small complete workflow over placeholder features.
