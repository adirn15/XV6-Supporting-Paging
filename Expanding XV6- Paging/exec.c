#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "defs.h"
#include "x86.h"
#include "elf.h"

int
exec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint argc, sz, sp, ustack[3+MAXARG+1];
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pde_t *pgdir, *oldpgdir;

  begin_op();
  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  pgdir = 0;

  // Check ELF header
  if(readi(ip, (char*)&elf, 0, sizeof(elf)) < sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pgdir = setupkvm()) == 0)
    goto bad;

/*** our code - in case no policy: backup and clear the page data **/
  #ifndef NONE

      /**** BACKUP AND CLEAR ****/
      int num_of_psyc_pages = proc->num_of_psyc_pages;
      proc->num_of_psyc_pages=0;

      int num_of_disk_pages = proc->num_of_disk_pages;
      proc->num_of_disk_pages=0;

      int num_of_page_faults = proc->num_of_page_faults;
      proc->num_of_page_faults=0;

      int total_num_of_paged_out = proc->total_num_of_paged_out;
      proc->total_num_of_paged_out=0;

      void * swapped_out_pages[SWAPFILESIZE];
      page_data in_use_pages[MAX_PSYC_PAGES];


      for (i=0; i<SWAPFILESIZE; i++){
        swapped_out_pages[i]= proc->swapped_out_pages[i];
        proc->swapped_out_pages[i]=(void*)-1;
      }
      for (i=0; i<MAX_PSYC_PAGES; i++){
        in_use_pages[i].va = proc->in_use_pages[i].va;
        proc->in_use_pages[i].va= (void*)-1;

        in_use_pages[i].accesses = proc->in_use_pages[i].accesses;
        proc->in_use_pages[i].accesses=-1;

        in_use_pages[i].creation_time = proc->in_use_pages[i].creation_time;
        proc->in_use_pages[i].creation_time=0;
      }

  #endif

  // Load program into memory.
  sz = 0;
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, (char*)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if((sz = allocuvm_paging(pgdir, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;
    if(loaduvm(pgdir, (char*)ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  // Allocate two pages at the next page boundary.
  // Make the first inaccessible.  Use the second as the user stack.
  sz = PGROUNDUP(sz);
  if((sz = allocuvm_paging(pgdir, sz, sz + 2*PGSIZE)) == 0)
    goto bad;
  clearpteu(pgdir, (char*)(sz - 2*PGSIZE));
  sp = sz;

  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp = (sp - (strlen(argv[argc]) + 1)) & ~3;
    if(copyout(pgdir, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[3+argc] = sp;
  }
  ustack[3+argc] = 0;

  ustack[0] = 0xffffffff;  // fake return PC
  ustack[1] = argc;
  ustack[2] = sp - (argc+1)*4;  // argv pointer

  sp -= (3+argc+1) * 4;
  if(copyout(pgdir, sp, ustack, (3+argc+1)*4) < 0)
    goto bad;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(proc->name, last, sizeof(proc->name));

  // Commit to the user image.
  oldpgdir = proc->pgdir;
  proc->pgdir = pgdir;
  proc->sz = sz;
  proc->tf->eip = elf.entry;  // main
  proc->tf->esp = sp;

/*** OUR CODE ***/
  // since the fork created the process but the swap file is not really relevant and needed

  removeSwapFile(proc);
  createSwapFile(proc);

// end our code 

  switchuvm(proc);
  freevm(oldpgdir);
  return 0;

 bad:
  if(pgdir)
    freevm(pgdir);
  if(ip){
    iunlockput(ip);
    end_op();
  }

/** our code **/

  #ifndef NONE //selection != NONE
  // RETURN PAGE INFORMATION TO PREVIOUS VALUE IN CASE OF FAIL
    proc->num_of_psyc_pages = num_of_psyc_pages;
    proc->num_of_disk_pages = num_of_disk_pages;
    proc->num_of_page_faults = num_of_page_faults;
    proc->total_num_of_paged_out=total_num_of_paged_out;

    for (i=0; i<SWAPFILESIZE; i++)
        proc->swapped_out_pages[i]= swapped_out_pages[i];
    
    for (i=0; i<MAX_PSYC_PAGES; i++){
      proc->in_use_pages[i].va = in_use_pages[i].va;
      proc->in_use_pages[i].accesses = in_use_pages[i].accesses;
      proc->in_use_pages[i].creation_time = in_use_pages[i].creation_time;
    }
  #endif

  return -1;
}
