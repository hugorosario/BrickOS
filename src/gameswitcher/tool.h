#ifndef BRICKOS_GAMESWITCHER_TOOL_H
#define BRICKOS_GAMESWITCHER_TOOL_H
// gs-session [--sd ROOT] --settings COMMAND, for Settings > GameSwitcher's menu.sh, through the
// entry's settings mode. It prints key=value lines:
//   get                 every setting in use, and valid=0 when settings.json is invalid
//   set KEY VALUE       sets one setting (gs_policy_set); refused=KEY when the rules refuse it.
//                       A new history_limit applies at once: removed=N.
//   info                games=N bytes=N play_ms=N (gs_history_info)
//   clear               cleared=N (gs_history_clear)
//   reset-time          reset=N (gs_history_reset_time)
// Returns 0, 1 on a failure or refusal, 2 for a usage error.
int gs_settings_command(const char *sd,const char *name,int argc,char **argv);
#endif
