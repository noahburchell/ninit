#include "del.h"
#include "add.h"

int cmd_del(int argc, char **argv)
{
	return svc_move(argc, argv, 0);
}
