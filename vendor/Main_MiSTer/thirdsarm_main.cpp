#include <sched.h>

#include "fpga_io.h"
#include "offload.h"
#include "thirdsarm_wrapper.h"

const char *version = "$VER:" VDATE;

int main(int argc, char *argv[])
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(1, &set);
	sched_setaffinity(0, sizeof(set), &set);

	offload_start();
	fpga_io_init();

	return thirdsarm_wrapper_run(argc, argv);
}
