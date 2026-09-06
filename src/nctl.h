#pragma once

#define NCTL_REQ_MAX	1024

#define NCTL_DATA	". "
#define NCTL_OK		"+ "
#define NCTL_ERR	"- "
#define NCTL_TAG_LEN	2

#define NCTL_IS_DATA(l)	((l)[0] == '.' && (l)[1] == ' ')
#define NCTL_IS_OK(l)	((l)[0] == '+' && (l)[1] == ' ')
#define NCTL_IS_ERR(l)	((l)[0] == '-' && (l)[1] == ' ')

#define NCTL_EXIT_OK	0
#define NCTL_EXIT_FAIL	1
#define NCTL_EXIT_USAGE	2
