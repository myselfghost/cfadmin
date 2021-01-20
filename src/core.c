#include "core.h"

#define LUALIBS_PATH \
	"lualib/?.lua;;lualib/?/init.lua;;" \
	"3rd/?.lua;;3rd/?/init.lua;;" \
	"script/?.lua;;script/?/init.lua;;"

#define LUACLIBS_PATH \
	"luaclib/?.so;;luaclib/lib?.so;;"         \
	"luaclib/?.dylib;;luaclib/lib?.dylib;;"   \
	"luaclib/?.dll;;luaclib/msys-?.dll;;"     \
																						\
	"./?.so;;./lib?.so;;"                     \
	"./?.dylib;;./lib?.dylib;;"               \
	"./?.dll;;./msys-?.dll;;"									\
																						\
  "3rd/?.so;;3rd/lib?.so;;"									\
  "3rd/?.dylib;;3rd/lib?.dylib;;"						\
  "3rd/?.dll;;3rd/msys-?.dll;;"

/* 忽略信号 */
static void SIG_IGNORE(core_loop *loop, core_signal *signal, int revents){
	return ;
}

/* 退出信号 */
static void SIG_EXIT(core_loop *loop, core_signal *signal, int revents){
	if (ev_userdata(loop) && core_get_watcher_userdata(signal)) {
		int index;
		pid_t *pids = (pid_t *)ev_userdata(loop);
		int pidcount = *(int*)core_get_watcher_userdata(signal);
		for (index = 0; index < pidcount; index++) {
			pid_t pid = pids[index];
			if (pid > 0)
				kill(pid, SIGKILL);
		}
	}
	_exit(0);
	return ;
}

/* 子进程退出则打印异常 */
static void CHILD_CB (core_loop *loop, ev_child *w, int revents){
	ev_child_stop(loop, w);
	ev_feed_signal_event(loop, SIGQUIT);
	if (w->rstatus){
		printf ("[WARNING]: sub process %d exited with signal: %d\n", w->rpid, w->rstatus);
		fflush(stdout);
	}
}

/* 内部异常 */
static void ERROR_CB(const char *msg){
	LOG("ERROR", msg);
	return ;
}

/* 为libev内存hook注入日志 */
static void *EV_ALLOC(void *ptr, long nsize){
	if (nsize == 0) return xfree(ptr), NULL;
	for (;;) {
		void *newptr = xrealloc(ptr, nsize);
		if (newptr) return newptr;
		LOG("WARN", "Allocate failed, Sleep sometime..");
		sleep(1);
	}
}

/* 为lua内存hook注入日志 */
static void* L_ALLOC(void *ud, void *ptr, size_t osize, size_t nsize){
	/* 用户自定义数据 */
	(void)ud;  (void)osize;
	if (nsize == 0) return xfree(ptr), NULL;
	for (;;) {
		void *newptr = xrealloc(ptr, nsize);
		if (newptr) return newptr;
		LOG("WARN", "Allocate failed, Sleep sometime..");
		sleep(1);
	}
}

void init_lua_libs(lua_State *L){
  /* lua 标准库 */
	luaL_openlibs(L);

	lua_pushglobaltable(L);
	lua_pushliteral(L, "null");
	lua_pushlightuserdata(L, NULL);
	lua_rawset(L, -3);
	lua_pushliteral(L, "NULL");
	lua_pushlightuserdata(L, NULL);
	lua_rawset(L, -3);

	lua_settop(L, 0);

	/* 注入lua搜索域 */
  lua_getglobal(L, "package");

	/* 注入lualib搜索路径 */
  lua_pushliteral(L, LUALIBS_PATH);
  lua_setfield(L, 1, "path");

	/* 注入luaclib搜索路径 */
	lua_pushliteral(L, LUACLIBS_PATH);
  lua_setfield(L, 1, "cpath");

  lua_settop(L, 0);

	/* 优化Lua的GC */
	CO_GCRESET(L);
}

/* 注册需要忽略的信号 */
core_signal sighup;
core_signal sigpipe;
core_signal sigtstp;

/* 注册需要退出的信号(docker需要) */
core_signal sigint;
core_signal sigterm;
core_signal sigquit;

static inline void signal_init(int* nprocess){

	/* 忽略连接中断信号 */
	core_signal_init(&sighup, SIG_IGNORE, SIGHUP);
	core_signal_start(CORE_LOOP_ &sighup);

	/* 忽略无效的管道读/写信号 */
	core_signal_init(&sigpipe, SIG_IGNORE, SIGPIPE);
	core_signal_start(CORE_LOOP_ &sigpipe);

	/* 忽略Ctrl-Z操作信号 */
	core_signal_init(&sigtstp, SIG_IGNORE, SIGTSTP);
	core_signal_start(CORE_LOOP_ &sigtstp);

	/* TERM信号 显示退出 */
	core_signal_init(&sigterm, SIG_EXIT, SIGTERM);
	core_signal_start(CORE_LOOP_ &sigterm);
	if (nprocess)
		core_set_watcher_userdata(&sigterm, nprocess);

	/* INT信号 显示退出 */
	core_signal_init(&sigint, SIG_EXIT, SIGINT);
	core_signal_start(CORE_LOOP_ &sigint);
	if (nprocess)
		core_set_watcher_userdata(&sigint, nprocess);

	/* QUIT信号 显示退出 */
	core_signal_init(&sigquit, SIG_EXIT, SIGQUIT);
	core_signal_start(CORE_LOOP_ &sigquit);
	if (nprocess)
		core_set_watcher_userdata(&sigquit, nprocess);

}

int core_worker_run(const char entry[]) {
	/* hook libev 内存分配 */
	core_ev_set_allocator(EV_ALLOC);
	/* hook 事件循环错误信息 */
	core_ev_set_syserr_cb(ERROR_CB);
	/* 初始化事件循环对象 */
	core_loop *loop = core_loop_fork(core_default_loop());

	int status = 0;

	lua_State *L = lua_newstate(L_ALLOC, NULL);
	if (!L)
		core_exit();

	init_lua_libs(L);

	status = luaL_loadfile(L, entry);
	if (status > 1){
		LOG("ERROR", lua_tostring(L, -1));
		lua_close(L);
		core_exit();
	}

	status = CO_RESUME(L, NULL, 0);
	if (status > 1){
		LOG("ERROR", lua_tostring(L, -1));
		lua_close(L);
		core_exit();
	}

	if (status == LUA_YIELD)
		signal_init(NULL);

	return core_start(loop, 0);
}

int core_master_run(pid_t *pids, int* pidcount) {
	/* hook libev 内存分配 */
	core_ev_set_allocator(EV_ALLOC);
	/* hook 事件循环错误信息 */
	core_ev_set_syserr_cb(ERROR_CB);
	/* 初始化信号 */ 
	signal_init(pidcount);
	/* 设置pid */ 
	ev_set_userdata(core_default_loop(), pids);
	/* 注册子进程监听 */
	ev_child childs[*pidcount];
	int index;
	for (index = 0; index < *pidcount; index++) {
		ev_child_init(&childs[index], CHILD_CB, pids[index], 0);
		ev_child_start(core_default_loop(), &childs[index]);
	}
	/* 初始化主进程 */ 
	return core_start(core_loop_fork(core_default_loop()), 0);
}