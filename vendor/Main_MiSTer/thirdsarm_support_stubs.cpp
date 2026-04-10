#include <stdint.h>

int arcade_get_direction()
{
	return 0;
}

int arcade_is_vertical()
{
	return 0;
}

void menu_key_set(unsigned int)
{
}

int menu_present()
{
	return 0;
}

void MenuHide()
{
}

void SelectINI()
{
}

void Info(const char *, int, int, int, int)
{
}

void InfoMessage(const char *, int, const char *)
{
}

void scheduler_yield()
{
}

char minimig_get_adjust()
{
	return 0;
}

void minimig_set_adjust(char)
{
}

void minimig_adjust_vsize(char)
{
}

void minimig_reset()
{
}

void tos_reset(char)
{
}

void archie_kbd(unsigned short)
{
}

void archie_mouse(unsigned char, int16_t, int16_t)
{
}

void x86_ide_set()
{
}

void x86_init()
{
}

void mcd_poll()
{
}

void mcd_reset()
{
}

void neocd_poll()
{
}

int neocd_is_en()
{
	return 0;
}

void neocd_reset()
{
}

void pcecd_poll()
{
}

void pcecd_reset()
{
}

void saturn_poll()
{
}

void saturn_reset()
{
}

void setBrightness(int, int)
{
}

void PrintDirectory(int)
{
}

void open_joystick_setup()
{
}

int menu_lightgun_cb(int, uint16_t, uint16_t, int)
{
	return 0;
}

int menu_allow_cfg_switch()
{
	return 0;
}

int xml_load(const char *)
{
	return 0;
}
