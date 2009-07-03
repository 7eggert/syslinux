/*
 * elf_module.c
 *
 *  Created on: Aug 11, 2008
 *      Author: Stefan Bucur <stefanb@zytor.com>
 */


#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <elf.h>

#include <linux/list.h>
#include <sys/module.h>

#include "elfutils.h"
#include "common.h"


static int check_header(Elf32_Ehdr *elf_hdr) {
	int res;

	res = check_header_common(elf_hdr);

	if (res != 0)
		return res;

	if (elf_hdr->e_type != MODULE_ELF_TYPE) {
		DBG_PRINT("The ELF file must be a shared object\n");
		return -1;
	}

	if (elf_hdr->e_phoff == 0x00000000) {
		DBG_PRINT("PHT missing\n");
		return -1;
	}

	return 0;
}

/*
 *
 * The implementation assumes that the loadable segments are present
 * in the PHT sorted by their offsets, so that only forward seeks would
 * be necessary.
 */
static int load_segments(struct elf_module *module, Elf32_Ehdr *elf_hdr) {
	int i;
	int res = 0;
	void *pht = NULL;
	Elf32_Phdr *cr_pht;

	Elf32_Addr min_addr  = 0x00000000; // Min. ELF vaddr
	Elf32_Addr max_addr  = 0x00000000; // Max. ELF vaddr
	Elf32_Word max_align = sizeof(void*); // Min. align of posix_memalign()
	Elf32_Addr min_alloc, max_alloc;   // Min. and max. aligned allocables

	Elf32_Addr dyn_addr = 0x00000000;

	// Get to the PHT
	image_seek(elf_hdr->e_phoff, module);

	// Load the PHT
	pht = malloc(elf_hdr->e_phnum * elf_hdr->e_phentsize);
	image_read(pht, elf_hdr->e_phnum * elf_hdr->e_phentsize, module);

	// Compute the memory needings of the module
	for (i=0; i < elf_hdr->e_phnum; i++) {
		cr_pht = (Elf32_Phdr*)(pht + i * elf_hdr->e_phentsize);

		switch (cr_pht->p_type) {
		case PT_LOAD:
			if (i == 0) {
				min_addr = cr_pht->p_vaddr;
			} else {
				min_addr = MIN(min_addr, cr_pht->p_vaddr);
			}

			max_addr = MAX(max_addr, cr_pht->p_vaddr + cr_pht->p_memsz);
			max_align = MAX(max_align, cr_pht->p_align);
			break;
		case PT_DYNAMIC:
			dyn_addr = cr_pht->p_vaddr;
			break;
		default:
			// Unsupported - ignore
			break;
		}
	}

	if (max_addr - min_addr == 0) {
		// No loadable segments
		DBG_PRINT("No loadable segments found\n");
		goto out;
	}

	if (dyn_addr == 0) {
		DBG_PRINT("No dynamic information segment found\n");
		goto out;
	}

	// The minimum address that should be allocated
	min_alloc = min_addr - (min_addr % max_align);

	// The maximum address that should be allocated
	max_alloc = max_addr - (max_addr % max_align);
	if (max_addr % max_align > 0)
		max_alloc += max_align;


	if (elf_malloc(&module->module_addr,
			max_align,
			max_alloc-min_alloc) != 0) {

		DBG_PRINT("Could not allocate segments\n");
		goto out;
	}

	module->base_addr = (Elf32_Addr)(module->module_addr) - min_alloc;
	module->module_size = max_alloc - min_alloc;

	// Zero-initialize the memory
	memset(module->module_addr, 0, module->module_size);

	for (i = 0; i < elf_hdr->e_phnum; i++) {
		cr_pht = (Elf32_Phdr*)(pht + i * elf_hdr->e_phentsize);

		if (cr_pht->p_type == PT_LOAD) {
			// Copy the segment at its destination
			if (cr_pht->p_offset < module->_cr_offset) {
				// The segment contains data before the current offset
				// It can be discarded without worry - it would contain only
				// headers
				Elf32_Off aux_off = module->_cr_offset - cr_pht->p_offset;

				if (image_read(module_get_absolute(cr_pht->p_vaddr, module) + aux_off,
						cr_pht->p_filesz - aux_off, module) < 0) {
					res = -1;
					goto out;
				}
			} else {
				if (image_seek(cr_pht->p_offset, module) < 0) {
					res = -1;
					goto out;
				}

				if (image_read(module_get_absolute(cr_pht->p_vaddr, module),
						cr_pht->p_filesz, module) < 0) {
					res = -1;
					goto out;
				}
			}

			DBG_PRINT("Loadable segment of size 0x%08x copied from vaddr 0x%08x at 0x%08x\n",
					cr_pht->p_filesz,
					cr_pht->p_vaddr,
					(Elf32_Addr)module_get_absolute(cr_pht->p_vaddr, module));
		}
	}

	// Setup dynamic segment location
	module->dyn_table = module_get_absolute(dyn_addr, module);

	DBG_PRINT("Base address: 0x%08x, aligned at 0x%08x\n", module->base_addr,
			max_align);
	DBG_PRINT("Module size: 0x%08x\n", module->module_size);

out:
	// Free up allocated memory
	if (pht != NULL)
		free(pht);

	return res;
}


static int prepare_dynlinking(struct elf_module *module) {
	Elf32_Dyn  *dyn_entry = module->dyn_table;

	while (dyn_entry->d_tag != DT_NULL) {
		switch (dyn_entry->d_tag) {
		case DT_NEEDED:
			// TODO: Manage dependencies here
			break;
		case DT_HASH:
			module->hash_table =
				(Elf32_Word*)module_get_absolute(dyn_entry->d_un.d_ptr, module);
			break;
		case DT_GNU_HASH:
			module->ghash_table =
				(Elf32_Word*)module_get_absolute(dyn_entry->d_un.d_ptr, module);
			break;
		case DT_STRTAB:
			module->str_table =
				(char*)module_get_absolute(dyn_entry->d_un.d_ptr, module);
			break;
		case DT_SYMTAB:
			module->sym_table =
				module_get_absolute(dyn_entry->d_un.d_ptr, module);
			break;
		case DT_STRSZ:
			module->strtable_size = dyn_entry->d_un.d_val;
			break;
		case DT_SYMENT:
			module->syment_size = dyn_entry->d_un.d_val;
			break;
		case DT_PLTGOT: // The first entry in the GOT
			module->got = module_get_absolute(dyn_entry->d_un.d_ptr, module);
			break;
		}

		dyn_entry++;
	}

	// Now compute the number of symbols in the symbol table
	if (module->ghash_table != NULL) {
		module->symtable_size = module->ghash_table[1];
	} else {
		module->symtable_size = module->hash_table[1];
	}

	return 0;
}


static int perform_relocation(struct elf_module *module, Elf32_Rel *rel) {
	Elf32_Word *dest = module_get_absolute(rel->r_offset, module);

	// The symbol reference index
	Elf32_Word sym = ELF32_R_SYM(rel->r_info);
	unsigned char type = ELF32_R_TYPE(rel->r_info);

	// The symbol definition (if applicable)
	Elf32_Sym *sym_def = NULL;
	struct elf_module *sym_module = NULL;
	Elf32_Addr sym_addr = 0x0;

	if (sym > 0) {
		// Find out details about the symbol

		// The symbol reference
		Elf32_Sym *sym_ref =
			(Elf32_Sym*)(module->sym_table + sym * module->syment_size);

		// The symbol definition
		sym_def =
			global_find_symbol(module->str_table + sym_ref->st_name,
					&sym_module);

		if (sym_def == NULL) {
			// This should never happen
			DBG_PRINT("Cannot perform relocation for symbol %s\n",
					module->str_table + sym_ref->st_name);

			return -1;
		}

		// Compute the absolute symbol virtual address
		sym_addr = (Elf32_Addr)module_get_absolute(sym_def->st_value, sym_module);

		if (sym_module != module) {
			// Create a dependency
			enforce_dependency(sym_module, module);
		}
	}

	switch (type) {
	case R_386_NONE:
		// Do nothing
		break;
	case R_386_32:
		*dest += sym_addr;
		break;
	case R_386_PC32:
		*dest += sym_addr - (Elf32_Addr)dest;
		break;
	case R_386_COPY:
		if (sym_addr > 0) {
			memcpy((void*)dest, (void*)sym_addr, sym_def->st_size);
		}
		break;
	case R_386_GLOB_DAT:
	case R_386_JMP_SLOT:
		// Maybe TODO: Keep track of the GOT entries allocations
		*dest = sym_addr;
		break;
	case R_386_RELATIVE:
		*dest += module->base_addr;
		break;
	default:
		DBG_PRINT("Relocation type %d not supported\n", type);
		return -1;
	}

	return 0;
}

static int resolve_symbols(struct elf_module *module) {
	Elf32_Dyn  *dyn_entry = module->dyn_table;
	unsigned int i;
	int res;

	Elf32_Word plt_rel_size = 0;
	void *plt_rel = NULL;

	void *rel = NULL;
	Elf32_Word rel_size = 0;
	Elf32_Word rel_entry = 0;

	// The current relocation
	Elf32_Rel *crt_rel;

	while (dyn_entry->d_tag != DT_NULL) {
		switch(dyn_entry->d_tag) {

		// PLT relocation information
		case DT_PLTRELSZ:
			plt_rel_size = dyn_entry->d_un.d_val;
			break;
		case DT_PLTREL:
			if (dyn_entry->d_un.d_val != DT_REL) {
				DBG_PRINT("Unsupported PLT relocation\n");
				return -1;
			}
		case DT_JMPREL:
			plt_rel = module_get_absolute(dyn_entry->d_un.d_ptr, module);
			break;

		// Standard relocation information
		case DT_REL:
			rel = module_get_absolute(dyn_entry->d_un.d_ptr, module);
			break;
		case DT_RELSZ:
			rel_size = dyn_entry->d_un.d_val;
			break;
		case DT_RELENT:
			rel_entry = dyn_entry->d_un.d_val;
			break;

		// Module initialization and termination
		case DT_INIT:
			// TODO Implement initialization functions
			break;
		case DT_FINI:
			// TODO Implement finalization functions
			break;
		}

		dyn_entry++;
	}

	if (rel_size > 0) {
		// Process standard relocations
		for (i = 0; i < rel_size/rel_entry; i++) {
			crt_rel = (Elf32_Rel*)(rel + i*rel_entry);

			res = perform_relocation(module, crt_rel);

			if (res < 0)
				return res;
		}

	}

	if (plt_rel_size > 0) {
		// TODO: Permit this lazily
		// Process PLT relocations
		for (i = 0; i < plt_rel_size/sizeof(Elf32_Rel); i++) {
			crt_rel = (Elf32_Rel*)(plt_rel + i*sizeof(Elf32_Rel));

			res = perform_relocation(module, crt_rel);

			if (res < 0)
				return res;
		}
	}

	return 0;
}



static int extract_operations(struct elf_module *module) {
	Elf32_Sym *init_sym = module_find_symbol(MODULE_ELF_INIT_PTR, module);
	Elf32_Sym *exit_sym = module_find_symbol(MODULE_ELF_EXIT_PTR, module);
	Elf32_Sym *main_sym = module_find_symbol(MODULE_ELF_MAIN_PTR, module);

	if (init_sym == NULL) {
		DBG_PRINT("Cannot find initialization routine pointer.\n");
		return -1;
	}
	if (exit_sym == NULL) {
		DBG_PRINT("Cannot find exit routine pointer.\n");
		return -1;
	}
	if (main_sym == NULL) {
		DBG_PRINT("Cannot find main routine pointer.\n");
		return -1;
	}

	module->init_func = (module_init_t*)module_get_absolute(
								init_sym->st_value, module);
	if (*(module->init_func) == NULL) {
		module->init_func = NULL;
	}

	module->exit_func = (module_exit_t*)module_get_absolute(
								exit_sym->st_value, module);
	if (*(module->exit_func) == NULL) {
		module->exit_func = NULL;
	}

	module->main_func = (module_main_t*)module_get_absolute(
								main_sym->st_value, module);
	if (*(module->main_func) == NULL) {
		module->main_func = NULL;
	}

	return 0;
}

// Loads the module into the system
int module_load(struct elf_module *module) {
	int res;
	Elf32_Ehdr elf_hdr;

	// Do not allow duplicate modules
	if (module_find(module->name) != NULL) {
		DBG_PRINT("Module already loaded.\n");
		return -1;
	}

	// Get a mapping/copy of the ELF file in memory
	res = image_load(module);

	if (res < 0) {
		return res;
	}

	// The module is a fully featured dynamic library
	module->shallow = 0;

	CHECKED(res, image_read(&elf_hdr, sizeof(Elf32_Ehdr), module), error);

	// Checking the header signature and members
	CHECKED(res, check_header(&elf_hdr), error);

	// Load the segments in the memory
	CHECKED(res, load_segments(module, &elf_hdr), error);
	// Obtain dynamic linking information
	CHECKED(res, prepare_dynlinking(module), error);

	// Check the symbols for duplicates / missing definitions
	CHECKED(res, check_symbols(module), error);

	// Obtain constructors and destructors
	CHECKED(res, extract_operations(module), error);

	// Add the module at the beginning of the module list
	list_add(&module->list, &modules_head);

	// Perform the relocations
	resolve_symbols(module);



	// The file image is no longer needed
	image_unload(module);

	DBG_PRINT("MODULE %s LOADED SUCCESSFULLY (init@0x%08X, exit@0x%08X)\n",
			module->name, *(module->init_func), *(module->exit_func));

	return 0;

error:
	// Remove the module from the module list (if applicable)
	list_del_init(&module->list);

	if (module->module_addr != NULL) {
		elf_free(module->module_addr);
		module->module_addr = NULL;
	}

	image_unload(module);

	return res;
}
