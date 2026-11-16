"""Guard the composed runtime menu's live/restart and media boundaries.

The provider ABI replaces the old standard-widget configuration. Authored
runtime menu and video-policy tests exercise those owners independently.
"""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
MAIN=(ROOT/'runtime/src/main.cpp').read_text(encoding='utf-8')
CDROM=(ROOT/'runtime/src/cdrom.c').read_text(encoding='utf-8')
PANEL=(ROOT/'runtime/src/psx_savestate_menu.c').read_text(encoding='utf-8')
TRAY=(ROOT/'runtime/src/psx_disc_tray.c').read_text(encoding='utf-8')
MENU=(ROOT/'runtime/src/psx_runtime_menu.c').read_text(encoding='utf-8')
def body(source,start,end):
 a=source.index(start);return source[a:source.index(end,a)]
# Provider edits are queued, then applied on the emulation thread. Resolution
# has a real live/recreate policy now; an unsupported edit remains deferred.
apply=body(MAIN,'unsigned mh_video_apply(', 'struct OvlModSet')
assert 'g_ovl_video_post = *next' in apply and 'g_ovl_video_pending = true' in apply
assert 'return mh_restart_bits(*next)' in apply
restart=body(MAIN,'unsigned mh_restart_bits(', 'unsigned mh_video_apply(')
assert 'PSX_VIDEO_RESTART_RESOLUTION' in restart and 'PSX_MENU_RESTART_AFTER_MATCH' in restart
live=body(MAIN,'extern "C" unsigned psx_video_apply_live(', 'static int host_resolution_apply_live(')
assert 'PSX_VIDEO_LIVE_FULLSCREEN' in live and 'psx_window_fullscreen_set' in live
assert 'video_resolution_apply_live(' in live and 'present_only' in live
host=MAIN[MAIN.index('static void host_pause_service(void) {'):]
assert 'g_ovl_video_pending = false' in host and 'psx_video_apply_live(&video)' in host
binding=body(MAIN,'PsxMenuHost g_menu_host = {','};')
assert 'mh_video_get, mh_video_apply' in binding
assert 'mh_launcher_settings_get' in binding and 'mh_launcher_settings_apply' in binding
# Keep the transactional direct replacement contract. The modern menu uses
# the roster/tray API instead of an unrestricted file picker.
swap=body(CDROM,'int cdrom_replace_disc','void cdrom_route_observe')
assert 'psx_netplay_active()' in swap
assert swap.index('replacement = iso_open') < swap.index('previous = iso_handle') < swap.index('iso_close(previous)')
assert 'debug_force_cd_reinsert();' in swap
request=body(MAIN,'int mh_disc_insert(', '#if defined(RECOMP_LAUNCHER)')
assert 'n < 1 || n > mh_disc_count()' in request and 'psx_netplay_active()' in request
assert 'g_ovl_disc_req = n' in request
assert 'g_ovl_disc_req.exchange(0)' in host and 'disc_cycle_now(disc)' in host
cycle=body(TRAY,'int psx_disc_tray_cycle(', 'int psx_disc_tray_auto_poll(')
assert cycle.index('psx_netplay_active()') < cycle.index('cdrom_disc_select(disc_1based)')
assert 'disc_1based > count' in cycle
insert=body(MENU,'int psx_runtime_menu_insert(void)', 'static int action(')
assert 'psx_disc_tray_cycle(s_disc, PSX_DISC_TRAY_ORIGIN_MENU)' in insert
assert 'write_cached_path' not in insert and 'save_user_settings' not in insert
assert 'recomp_runtime_ui_render_argb8888' in PANEL and 'recomp_runtime_ui_render_argb8888' in MENU
print('PASS: composed provider live/restart, main-thread media and direct-swap guards')
