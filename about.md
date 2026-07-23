# Custom Icons

Upload PNG files as switchable **skins/variations** for any icon.

## How to use

1. In the **Icon Kit**, equip the icon you want to skin, then press the **Skins** button (top left).
2. In the popup, use the arrows to pick the gamemode - it targets your currently equipped icon of that type (the sheet name is shown, e.g. `player_01`).
3. Press **Add Image** and pick a PNG spritesheet, then choose the plist source:
   - **Existing**: uses the game's own plist. Your PNG must be a drop-in replacement for the icon's original sheet (same layout and size as e.g. `Resources/icons/player_01-uhd.png`).
   - **My Own**: also pick a `.plist` file (e.g. exported from TexturePacker). Its frames must use the game's frame names (`player_01_001.png`, `player_01_2_001.png`, `player_01_glow_001.png`, `player_01_extra_001.png`, ...).
4. Click any entry in the list to switch between **Default** and your variations. Switching applies instantly to the preview and gameplay; the garage's icon-grid thumbnails refresh the next time you open the Icon Kit.

## Notes

- Make your sheet for the texture quality you play at (UHD recommended).
- Variations are stored per icon (per sheet), in this mod's save folder - delete a variation's files there to remove it from the list.
