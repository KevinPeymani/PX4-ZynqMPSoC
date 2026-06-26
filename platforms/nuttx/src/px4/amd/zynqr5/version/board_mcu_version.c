#include <px4_platform_common/px4_config.h>
#include <px4_platform_common/defines.h>

/* Zynq UltraScale+ MPSoC Device Identity Code Register (CSU_IDCODE) */
#define ZYNQMP_CSU_IDCODE_ADDR  0xFFCA4040U
#define ZYNQMP_CSU_VERSION_ADDR 0xFFCA4044U

int board_mcu_version(char *rev, const char **revstr, const char **errata)
{
	*revstr  = "Zynq UltraScale+ MPSoC R5F";
	*rev     = '1';

	if (errata) {
		*errata = NULL;
	}

	return 0;
}
