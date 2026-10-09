# Moorhuhn

Shoot chickens and special targets to score points in a 90-second round.
You have eight shots per magazine. Reload when it is empty.

## Start

Extract the entire archive before starting:

- **Linux:** run `./moorhuhn`. Nightly builds require x86_64 and glibc 2.39+.
  Local builds require the build machine's glibc version or newer. The ZIP with
  `-bundled` includes the compiler runtimes; the smaller ZIP uses system runtimes.
- **Windows:** run `moorhuhn.exe` with the supplied DLLs in the same folder.
- **macOS 13+:** open `Moorhuhn.app`. Choose arm64 for Apple Silicon or x86_64 for Intel.
  The app is ad-hoc signed.

Your name and highscores save automatically in your user profile.

## Controls

- **Space:** start a game.
- **Enter:** confirm your name.
- **Mouse:** aim.
- **Left click:** shoot.
- **Right click:** reload an empty magazine.
- **Left/Right arrows or screen edges:** move the camera.
- **Escape:** end the round; at the title, exit through the credits.
