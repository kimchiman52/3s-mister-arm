#include <stdint.h>

namespace {

volatile unsigned int g_menu_key = 0;
volatile int g_menu_visible = 0;

}

int arcade_get_direction()
{
	return 0;
}

int arcade_is_vertical()
{
	return 0;
}

void menu_key_set(unsigned int c)
{
	g_menu_key = c;
}

unsigned int threesx_wrapper_menu_key_take()
{
	unsigned int key = g_menu_key;
	g_menu_key = 0;
	return key;
}

void threesx_wrapper_menu_set_visible(int visible)
{
	g_menu_visible = visible ? 1 : 0;
}

int menu_present()
{
	return g_menu_visible;
}

void MenuHide()
{
	threesx_wrapper_menu_set_visible(0);
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
