/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Added to the default script of the micropython extension with a display. The kernel's iterable
 * sections its drivers use, which stay inside the extension as the input subsystem does.
 */
SECTIONS
{
	.mp_input_callbacks : ALIGN(4)
	{
		_input_callback_list_start = .;
		KEEP(*(SORT_BY_NAME(._input_callback.static.*)));
		_input_callback_list_end = .;
	}
}
INSERT AFTER .got;
