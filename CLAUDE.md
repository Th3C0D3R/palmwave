# Palmwave / Moon AI

Read AGENTS.md and CONTRIBUTING.md before working here. Before implementation, also read docs/boot.md, docs/runtime_imports.md and docs/d3d12_vulkan.md. Follow the original first-frame objective and the running task's current instructions.

- Write project documentation and commit messages in English.
- Palmwave is developed under Moon AI (https://realmoon.ai), the AI platform created by Stellarr Studio.
- For authorized new project commits, use the project credit `Co-authored-by: Moon AI <contact@stellarrstudio.com>` and retain accurate source and tool attribution. This is the platform's project identity, not a claim that a different provider's model was used.
- Preserve GPL-3.0 source credits and third-party notices. Record the source revision and license for borrowed code. Never copy incompatible GPL-2.0-only implementations.
- Keep game files, generated shaders, private traces and build outputs outside the authoring tree. Do not access or send game data to an AI service.
- Work from measured blockers. Fix one cause, add a focused synthetic regression when appropriate, and record the actual tests and GPU/driver. Do not suppress shaders or draws to imply rendering progress.
- Do not edit or commit while assigned a read-only review. Coordinate file ownership with the primary agent before parallel implementation.
- The target is a verified game frame. A successful Present call or a black swapchain does not establish that milestone.
