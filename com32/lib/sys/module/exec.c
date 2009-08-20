/*
 * exec.c
 *
 *  Created on: Aug 14, 2008
 *      Author: Stefan Bucur <stefanb@zytor.com>
 */

#include <sys/module.h>
#include <sys/exec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <setjmp.h>
#include <alloca.h>

#define DBG_PRINT(fmt, args...)	fprintf(stderr, "[EXEC] " fmt, ##args)

static struct elf_module    *mod_root = NULL;
struct elf_module *__syslinux_current = NULL;

int exec_init(void)
{
	int res;

	res = modules_init();

	if (res != 0)
		return res;

	// Load the root module
	mod_root = module_alloc(EXEC_ROOT_NAME);

	if (mod_root == NULL)
		return -1;

	res = module_load_shallow(mod_root, 0);

	if (res != 0) {
		mod_root = NULL;
		return res;
	}

	return 0;
}

int get_module_type(struct elf_module *module)
{
	if(module->main_func) return EXEC_MODULE;
	else if(module->init_func) return LIB_MODULE;
	return UNKNOWN_MODULE;
}

int load_library(const char *name)
{
	int res;
	struct elf_module *module = module_alloc(name);

	if (module == NULL)
		return -1;

	res = module_load(module);


	if (res != 0) {
		module_unload(module);
		return res;
	}

	if (module->main_func != NULL) {
		DBG_PRINT("Cannot load executable module as library.\n");
		module_unload(module);
		return -1;
	}

	if (module->init_func != NULL) {
		res = (*(module->init_func))();
		DBG_PRINT("Initialization function returned: %d\n", res);
	} else {
		DBG_PRINT("No initialization function present.\n");
	}

	if (res != 0) {
		module_unload(module);
		return res;
	}

	return 0;
}

int unload_library(const char *name)
{
	int res;
	struct elf_module *module = module_find(name);

	if (module == NULL)
		return -1;

	if (!module_unloadable(module)) {
		return -1;
	}

	if (module->exit_func != NULL) {
		(*(module->exit_func))();
	}

	res = module_unload(module);

	return res;
}

jmp_buf __process_exit_jmp;

int spawnv(const char *name, const char **argv)
{
	int res, ret_val = 0;
	const char **arg;
	int argc;
	char **argp, **args;
	struct elf_module *previous;
	malloc_tag_t prev_mem_tag;

	struct elf_module *module = module_alloc(name);

	if (module == NULL)
		return -1;

	res = module_load(module);

	if (res != 0) {
		module_unload(module);
		return res;
	}
	
	if (module->main_func == NULL) {
		// We can't execute without a main function
		module_unload(module);
		return -1;
	}
	/*if (module->main_func != NULL) {
		const char **last_arg = argv;
		void *old_tag;
		while (*last_arg != NULL)
			last_arg++;

		// Setup the memory allocation context
		old_tag = __mem_get_tag_global();
		__mem_set_tag_global(module);

		// Execute the program
		ret_val = (*(module->main_func))(last_arg - argv, argv);

		// Clean up the allocation context
		__free_tagged(module);
		// Restore the allocation context
		__mem_set_tag_global(old_tag);
	} else {
		// We can't execute without a main function
		module_unload(module);
		return -1;
	}*/
	// Set up the process context
	previous = __syslinux_current;
	prev_mem_tag = __mem_get_tag_global();

	// Setup the new process context
	__syslinux_current = module;
	__mem_set_tag_global((malloc_tag_t)module);

	// Generate a new process copy of argv (on the stack)
	argc = 0;
	for (arg = argv; *arg; arg++)
		argc++;

	args = alloca((argc+1) * sizeof(char *));
	
	for (arg = argv, argp = args; *arg; arg++, argp++) {
		size_t l = strlen(*arg)+1;
		*argp = alloca(l);
               	memcpy(*argp, *arg, l);
 	}

	*args = NULL;

	// Execute the program
	ret_val = setjmp(module->u.x.process_exit);

	if (ret_val)
		ret_val--;		/* Valid range is 0-255 */
	else if (!module->main_func)
		ret_val = -1;
	else
		exit((*module->main_func)(argc, args)); /* Actually run! */
	
	// Clean up the allocation context
	__free_tagged(module);
	// Restore the allocation context
	__mem_set_tag_global(prev_mem_tag);
	// Restore the process context
	__syslinux_current = previous;

	res = module_unload(module);

	if (res != 0) {
		return res;
	}

	return ((unsigned int)ret_val & 0xFF);
}

int spawnl(const char *name, const char *arg, ...)
{
	/*
	 * NOTE: We assume the standard ABI specification for the i386
	 * architecture. This code may not work if used in other
	 * circumstances, including non-variadic functions, different
	 * architectures and calling conventions.
	 */

	return spawnv(name, &arg);
}

int spawn_load(const char *name,const char **argv)
{
	int res, ret_val = 0;
	const char **arg;
	int argc;
	char **argp, **args;
	struct elf_module *previous;
	malloc_tag_t prev_mem_tag;
	struct elf_module *module = module_alloc(name);
	
	int type;

	if (module == NULL)
		return -1;

	res = module_load(module);


	if (res != 0) {
		module_unload(module);
		return res;
	}

	type=get_module_type(module);
	
	if(type==LIB_MODULE)
	{
		if (module->init_func != NULL) {
			res = (*(module->init_func))();
			DBG_PRINT("Initialization function returned: %d\n", res);
		} else {
			DBG_PRINT("No initialization function present.\n");
		}

		if (res != 0) {
			module_unload(module);
			return res;
		}
		return 0;
	}
	else if(type==EXEC_MODULE)
	{
		previous = __syslinux_current;
		prev_mem_tag = __mem_get_tag_global();

		// Setup the new process context
		__syslinux_current = module;
		__mem_set_tag_global((malloc_tag_t)module);

		// Generate a new process copy of argv (on the stack)
		argc = 0;
		for (arg = argv; *arg; arg++)
			argc++;

		args = alloca((argc+1) * sizeof(char *));
	
		for (arg = argv, argp = args; *arg; arg++, argp++) {
			size_t l = strlen(*arg)+1;
			*argp = alloca(l);
        	       	memcpy(*argp, *arg, l);
 		}

		*args = NULL;

		// Execute the program
		ret_val = setjmp(module->u.x.process_exit);
	
		if (ret_val)
			ret_val--;		/* Valid range is 0-255 */
		else if (!module->main_func)
			ret_val = -1;
		else
			exit((*module->main_func)(argc, args)); /* Actually run! */
		
		// Clean up the allocation context
		__free_tagged(module);
		// Restore the allocation context
		__mem_set_tag_global(prev_mem_tag);
		// Restore the process context
		__syslinux_current = previous;
	
		res = module_unload(module);
	
		if (res != 0) {
			return res;
		}

		return ((unsigned int)ret_val & 0xFF);
	}
	module_unload(module);
	return -1;
}

void exec_term(void)
{
	modules_term();
}

