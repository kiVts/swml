5.0.0: New mod window on every platform: Dear ImGui + SDL3 instead of Win32 common controls. On Linux it is a native Wayland/X11 app (HiDPI, desktop file picker and trash, no Wine UI); Play hands the game to Proton, which runs swml.exe --run headless to load the core. The launcher logic (mods, settings, updates, zip) is now portable and covered by unit tests; INI/settings files keep the exact Windows format so the Proton core reads them unchanged. Windows keeps the same workflow; the in-game core and startup bootstrap are unchanged.

4.5.1: The settings panel shows settings measured in a unit (seconds, ms, KB, ...) as number fields, even when their default is 0 or 1. Before, Physics Fixes' isolate_s showed as a checkbox that could only save 1.

4.5.0: Changes the launch method to possibly stop the Windows Defender from picking up SWML as a virus.

4.4.0: Added an in-loader ability to change all settings without needing to open a text file. Update the SDK to better support the in-game text.
