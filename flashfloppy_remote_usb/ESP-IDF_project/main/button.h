#pragma once

/* BOOT button (v42): FlashFloppy-style image browsing on the
 * board's BOOT button (GPIO0), plus the LCD screensaver.
 *
 * ff_button_start() spawns the poll task. A press steps a preview
 * to the next image in the library — LCD only; nothing touches
 * the card or the Gotek while browsing. After
 * FF_PREVIEW_COMMIT_MS without a further press, the previewed
 * image is selected through the same request path as the web UI.
 * A web-UI request or a Gotek-side selection cancels the preview.
 * While the drive is ejected, presses do nothing but register as
 * activity.
 *
 * The screensaver blanks the LCD after FF_LCD_SCREENSAVER_MS
 * without activity (a press, a selection change, a swap). Any
 * press wakes the screen — and is consumed by the wake, so it
 * does not also step the preview (the rule Kris chose for the
 * MFM OLED daemon). */
void ff_button_start(void);
