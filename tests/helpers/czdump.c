// prints a service file as ninitctl init stores it

#include "../../ctl/build.c"

int main(int argc, char **argv)
{
	struct src s = { 0 };
	const char *name;
	size_t len;
	char *buf;

	if (argc < 2 || argc > 3) {
		fputs("Usage: czdump FILE [NAME]\n", stderr);
		return 2;
	}
	name = argc == 3 ? argv[2] : strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1];
	g_dir = ".";
	buf = slurp(argv[1], &len);
	parse_src(&s, name, buf, len);
	if (s.script)
		fwrite(s.script, 1, strlen(s.script), stdout);
	return 0;
}
