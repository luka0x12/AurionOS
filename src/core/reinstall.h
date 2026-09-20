#ifndef REINSTALL_H
#define REINSTALL_H
int cmd_reinstall(const char *args);
/* Called by sudo before its root shortcut or cached authorization check. */
int cmd_reinstall_from_sudo(const char *args);
#endif
