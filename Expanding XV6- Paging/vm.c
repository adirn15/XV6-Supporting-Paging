#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

#define ONLY_ADDED 1
#define SWAPPED_ADDED 2
#define NOUPDATE 0
#define INITPROC 1
#define SHELLPROC 2
#define MAXINTEGER 35000

/*our functions*/
int allocuvm_paging(pde_t * pgdir, uint oldsz, uint newsz);

int free_place_in_RAM(pde_t*);
void free_place_lifo(pde_t * pgdir,int isfull);
void free_place_scfifo(pde_t * pgdir,int isfull);
void free_place_lap(pde_t * pgdir,int isfull);

int find_free_swapfile_index(void);
void update_creation_time(int index);
void load_page_from_swapfile_to_RAM(void* vaddr,int extra_slot);
void write_from_RAM_to_swapfile(pde_t* pgdir, int swapfile_index,int page_index);
void remove_page_from_swapfile(int pgdir, int a);
void remove_page_from_RAM(int pgdir,int a);
void add_page_to_RAM(int pgdir,int a);
int passed_memory_limit(int id,int size);
int exists_space_in_physical(struct proc* p);

int is_paged_out(uint add,uint pde);
void update_accessess(void);

void print_page_ds();


extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()
struct segdesc gdt[NSEGS];

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpunum()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);

  // Map cpu, and curproc
  c->gdt[SEG_KCPU] = SEG(STA_W, &c->cpu, 8, 0);

  lgdt(c->gdt, sizeof(c->gdt));
  loadgs(SEG_KCPU << 3);
  
  // Initialize cpu-local storage.
  cpu = c;
  proc = 0;
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)p2v(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table 
    // entries, if necessary.
    *pde = v2p(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;
  
  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
// 
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP, 
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (p2v(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start, 
                (uint)k->phys_start, k->perm) < 0)
      return 0;
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(v2p(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  pushcli();
  cpu->gdt[SEG_TSS] = SEG16(STS_T32A, &cpu->ts, sizeof(cpu->ts)-1, 0);
  cpu->gdt[SEG_TSS].s = 0;
  cpu->ts.ss0 = SEG_KDATA << 3;
  cpu->ts.esp0 = (uint)proc->kstack + KSTACKSIZE;
  ltr(SEG_TSS << 3);
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");
  lcr3(v2p(p->pgdir));  // switch to new address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;
  
  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, v2p(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, p2v(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}



//pid1 = init, pid2= shell, pid>2= user processes
int
allocuvm_paging(pde_t *pgdir, uint oldsz, uint newsz){
  // if SELECTION = NONE , no page replacement policy exists
  if (proc==0 || proc->pid<=2)
    return allocuvm(pgdir,oldsz,newsz);

  #ifdef NONE
    return allocuvm(pgdir,oldsz,newsz);
  #endif

  char *mem;
  uint a;
  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){

    if (passed_memory_limit(proc->pid,a))       //case: reached max total pages
      panic("allocuvm: reached max total pages, out of memory\n");

    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }

    if(proc->pid>2 && !exists_space_in_physical(proc)){ //case: need to remove file from physical
      //cprintf("freeing memory in RAM\n");
      free_place_in_RAM(pgdir);
    }
    memset(mem, 0, PGSIZE);
    mappages(pgdir, (char*)a, PGSIZE, v2p(mem), PTE_W|PTE_U);
    
    add_page_to_RAM((int)pgdir,a);
    
    #ifdef TRUE
      print_page_ds();
    #endif
  }
  return newsz;
}


void add_page_to_RAM(int pgdir,int a){
  int i;

  for (i=0; i<MAX_PSYC_PAGES; i++){
    if (proc->in_use_pages[i].va==(void*)-1){ //find free spot in physical
        proc->in_use_pages[i].va= (void*)a;
        #ifndef LAP
          update_creation_time(i);
        #endif
        proc->in_use_pages[i].accesses = 0;
        proc->num_of_psyc_pages++;
        break;
    }
  }
}

int free_place_in_RAM(pde_t* pgdir){
  int isfull=0;
  if (proc->num_of_disk_pages+proc->num_of_psyc_pages == MAX_TOTAL_PAGES)
    isfull=1;

  #ifdef NONE
      return;
  #elif LIFO
        free_place_lifo(pgdir,isfull); //remove from physical memory lifo (is full is meant to swapfile disk)
  #elif SCFIFO
        free_place_scfifo(pgdir,isfull); //remove from physical memory scfifo
  #elif LAP
        free_place_lap(pgdir,isfull); //remove from physical memory lap
  #endif
  lcr3(v2p(proc->pgdir));  // refresh TLB
  return isfull;
}

int passed_memory_limit(int id,int size){
  if (proc==0)
    return 0;
  #ifndef NONE
  if (id>2 && (proc->num_of_psyc_pages+proc->num_of_disk_pages)>=MAX_TOTAL_PAGES)
    return 1;
  #endif
  return 0;
}
int exists_space_in_physical(struct proc* p){
  if (p->pid>2 && p->num_of_psyc_pages<15)
    return 1;
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    mappages(pgdir, (char*)a, PGSIZE, v2p(mem), PTE_W|PTE_U);
  }
  return newsz;
}


// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);

    if(!pte)
      a += (NPTENTRIES - 1) * PGSIZE;

    else if((*pte & PTE_P) != 0){ //page is in RAM
        pa = PTE_ADDR(*pte);
        if(pa == 0)
          panic("kfree");

        if (proc->pgdir == pgdir){
        #ifndef NONE
          if(proc->pid!=INITPROC && proc->pid!=SHELLPROC)
            remove_page_from_RAM((int)pgdir,a);
        #endif

        }

        char *v = p2v(pa);
        kfree(v);
        *pte = 0;

    }

    else if ((*pte & PTE_PG)!=0 && proc->pgdir==pgdir){ //page is in file
      #ifndef NONE
        remove_page_from_swapfile((int)pgdir,a);
      #endif
    }
  }
  return newsz;
}

void remove_page_from_RAM(int pgdir,int a){
  int i;
  for (i=0; i<MAX_PSYC_PAGES; i++){
    if (proc->in_use_pages[i].va==(void*)a){
      proc->in_use_pages[i].va=(void*)-1;
      proc->in_use_pages[i].accesses=-1;
      proc->in_use_pages[i].creation_time=0;
      proc->num_of_psyc_pages--;
      return;
    }
  }
}

void remove_page_from_swapfile(int pgdir, int a){
  int i;
  int va = PTE_ADDR(a);
  for (i=0; i<SWAPFILESIZE; i++){
    if (proc->swapped_out_pages[i]==(void*)va){
      proc->swapped_out_pages[i]=(void*)-1;
      proc->num_of_disk_pages--;
      break;
    }
  }
  if (i==SWAPFILESIZE)
    panic("remove_page_from_swapfile: could not find swapfile page!\n");

  pte_t * pte = walkpgdir((pde_t*)pgdir, (char*)a, 0);
  *pte = *pte & (~PTE_PG); //clear page fault flag

}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = p2v(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  pte_t* pte2;
  uint pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P) && !(*pte & PTE_PG)) //not in RAM or DISK 
      panic("copyuvm: page not present");
    if((*pte & PTE_P)){ // if in RAM
      pa = PTE_ADDR(*pte);
      flags = PTE_FLAGS(*pte);
      if((mem = kalloc()) == 0)
        goto bad;
      memmove(mem, (char*)p2v(pa), PGSIZE);
      if(mappages(d, (void*)i, PGSIZE, v2p(mem), flags) < 0)
        goto bad;
    }
    //our code
    else if (!(*pte & PTE_P) && (*pte & PTE_PG)){ //if in DISK
      if((pte2 = walkpgdir(d, (void*)i, 1)) == 0) 
        goto bad;
      if(*pte2 & PTE_P)
        panic("remap");
      *pte2 = *pte;
    }
    
  }
  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)p2v(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}


//store on swapfile the lifo process and free his space in memory - RETURN VALUE = WAS IT WRITTEN IN THE EXTRA SPOT (1) OR NOT (0)
void free_place_lifo(pde_t* pgdir, int full){ 
  /* find last created page */
  int i;
  int LIFOindex = -1;
  for (i=0; i<MAX_PSYC_PAGES; i++){
    if (proc->in_use_pages[i].va == (void*)-1)
      continue;
    else{
      LIFOindex=i;
      break;
    }
  }
  if (LIFOindex==-1)
    panic("free_place_lifo: nothing to free\n");

  for (i=LIFOindex; i<MAX_PSYC_PAGES; i++){
    if (proc->in_use_pages[i].va == (void*)-1)
      continue;
    if (proc->in_use_pages[i].creation_time > proc->in_use_pages[LIFOindex].creation_time)
      LIFOindex= i;
  }
  if (LIFOindex == -1)
    panic("vm lifo error: no pages allocated\n");

    //cprintf("FREEING LIFO PAGE INDEX FROM RAM: %d\n",LIFOindex);
    proc->in_use_pages[LIFOindex].creation_time=0;
    proc->in_use_pages[LIFOindex].accesses=-1;
  
  if(!full){
    /*find empty place in swap file*/
    int swapfile_index =  find_free_swapfile_index();
    //cprintf("FOUND FREE SLOT IN FILE: %d\n",swapfile_index);
    write_from_RAM_to_swapfile(pgdir, swapfile_index,LIFOindex);
  }
  else{
    write_from_RAM_to_swapfile(pgdir, 15,LIFOindex); //save at the end of the file
  }
  
}

//store on swapfile the scfifo process and free his space in memory
//return index of freed space
void free_place_scfifo(pde_t* pgdir, int isfull){

  int sorted_pages[MAX_PSYC_PAGES]; //page indexes sorted by creation time
  int already_added[MAX_PSYC_PAGES]; // helper array to sort the pages
  
  int i,j;

  for(i=0; i<MAX_PSYC_PAGES; i++)
    sorted_pages[i]=-1;
  for(i=0; i<MAX_PSYC_PAGES; i++)
    already_added[i]=0;
  
  // sort page indexes by access into sorted_pages
  for(i=0; i<MAX_PSYC_PAGES; i++){

    int min=MAXINTEGER;
    int minindex = -1;

    for(j=0; j<MAX_PSYC_PAGES; j++){
      if (proc->in_use_pages[j].creation_time==-1 || already_added[j]==1) 
        continue;
      if (proc->in_use_pages[j].creation_time < min){
        min = proc->in_use_pages[j].creation_time;
        minindex= j;
        already_added[j]=1;
      }
    }
    if (min!=MAXINTEGER)
      sorted_pages[i]=minindex;
  }

  int selected_index_to_file=-1;
  int cur_index;

  /* GO OVER SORTED Array AND CHECK PTE_A */
  for (i=0; i<MAX_PSYC_PAGES; i++){
    cur_index = sorted_pages[i];
    #ifdef TRUE
      cprintf("################################## minimum index is: %d\n",cur_index);
    #endif
    if (cur_index==-1)
      continue;
    char* cur_address=(char*)(proc->in_use_pages[cur_index].va);
    pte_t * cur_pte = walkpgdir(pgdir, cur_address, 0);         
    if (!(*cur_pte & PTE_A)){ // if access flag = 0
      selected_index_to_file = cur_index;
      break;
    }
    else{ //if accessed: 
      update_creation_time(cur_index); //update creation time to get to end of line
      *cur_pte = (*cur_pte) & (~PTE_A); //clear access flag
    }
  }

  //if all pages were accessed - take first and clear his access flag
  if (selected_index_to_file ==-1){
    cur_index=sorted_pages[0];
    char* cur_address = (char*)(cur_index*PGSIZE);
    pte_t * cur_pte = walkpgdir(pgdir, cur_address, 0); 
    *cur_pte = (*cur_pte) & (~PTE_A); //clear access flag
  }

  proc->in_use_pages[selected_index_to_file].creation_time=0;
  proc->in_use_pages[selected_index_to_file].accesses=-1;

  if (!isfull){
    int swapfile_index =  find_free_swapfile_index();
    write_from_RAM_to_swapfile(pgdir, swapfile_index,selected_index_to_file);  
  }
  else{
    write_from_RAM_to_swapfile(pgdir,15,selected_index_to_file);  
  }
}

//store on swapfile the lap process and free his space in memory
void free_place_lap(pde_t* pgdir, int isfull){

  int i;
  int LAPindex = -1;
  int minaccess = MAXINTEGER;

  /* find least accessed page */
  for (i=0; i<MAX_PSYC_PAGES; i++){
    int cur_access = proc->in_use_pages[i].accesses;
    if (cur_access!=-1 && minaccess > cur_access){
      LAPindex= i;
      minaccess= cur_access;
    }
  }
  if (LAPindex == MAXINTEGER)
    panic("vm LAP error: no pages allocated\n");
  proc->in_use_pages[LAPindex].creation_time=0;
  proc->in_use_pages[LAPindex].accesses=-1;

  if (!isfull){
     int swapfile_index =  find_free_swapfile_index();
     write_from_RAM_to_swapfile(pgdir,swapfile_index,LAPindex);
  }
  else{
    write_from_RAM_to_swapfile(pgdir,15,LAPindex);
  }
}

void write_from_RAM_to_swapfile(pde_t* pgdir, int swapfile_index,int page_index){
  
  int write_offset = swapfile_index*PGSIZE;                            //offset to write to in swapfile
  char* to_file_va = (char*)(proc->in_use_pages[page_index].va);        //get virtual address of page
  proc->swapped_out_pages[swapfile_index] = (void*)to_file_va;           //update swapfile info
  writeToSwapFile(proc,to_file_va,write_offset,PGSIZE);           //write to swapfile
  proc->num_of_disk_pages++;
  proc->num_of_psyc_pages--;
  proc->total_num_of_paged_out++;
  proc->in_use_pages[page_index].va=(void*)-1;
  //change flags accordingly of the stored page
  pte_t * stored_pte = walkpgdir(pgdir,to_file_va,0);  
  *stored_pte = (*stored_pte)|(PTE_PG);  //add pg fault flag
  *stored_pte = (*stored_pte)&(~PTE_P);  //clear present flag
  
  kfree((char*)PTE_ADDR(p2v(*stored_pte)));   
  lcr3(v2p(pgdir));
  //free the lifo memory after storing on file
}


void update_creation_time(int pgindex){
  int i;
  int min = MAXINTEGER;
  int max = 0;
  
  //find min and max born time
  for (i=0; i< MAX_PSYC_PAGES; i++){
    int cur_creation_time = proc->in_use_pages[i].creation_time;
    if (cur_creation_time!=0 && cur_creation_time > max)
      max = cur_creation_time;
    if (cur_creation_time!=0 && cur_creation_time < min)
      min = cur_creation_time;
  }
  if (max==0 && min==MAXINTEGER){
    proc->in_use_pages[pgindex].creation_time=1;
    return;
  }
  // always keep born time in range 
  if (min>MAX_PSYC_PAGES){ 
    for (i=0; i< MAX_PSYC_PAGES; i++)
      if (proc->in_use_pages[i].creation_time!=0)
        proc->in_use_pages[i].creation_time-=MAX_PSYC_PAGES;
    max-=MAX_PSYC_PAGES; // all creation time reduce by 15: t=t-15
  }
  int newtime = max+1;
  proc->in_use_pages[pgindex].creation_time= newtime; 
}

int find_free_swapfile_index(void)
{
  int i;
  for (i = 0; i < SWAPFILESIZE; i++){
    if (proc->swapped_out_pages[i] == (void*)-1)
      return i;
  }
  panic("vm: no free place in swap file found\n");
  return -1;
}


void load_page_from_swapfile_to_RAM(void* vaddr, int extra_slot){
  
  uint addr_page = PGROUNDDOWN((uint)vaddr);

  //find the wanted page in the swapped pages array
  int i;
  for (i=0; i<SWAPFILESIZE; i++){ 
    if (proc->swapped_out_pages[i]==(void*)addr_page){
      proc->swapped_out_pages[i]=(void*)-1;
      break;
    }
  }
  if (i==SWAPFILESIZE) //didnt find the wanted page
    panic("load_page_from_swapfile_to_RAM: page not found in file\n");

  //find a free place in in_use_pages array
  int j;
  for (j=0; j<MAX_PSYC_PAGES; j++){
    if (proc->in_use_pages[j].va==(void*)-1){
      proc->in_use_pages[j].va=(void*)addr_page;
      update_creation_time(j);
      proc->in_use_pages[j].accesses = 0;
      break;
    }
  }
  if (j==MAX_PSYC_PAGES)
    panic("load_page_from_swapfile_to_RAM: no free spot in psyc_pages\n");

  char *mem= kalloc();  
  if(mem == 0){
    panic("memory allocation failed");
    return;
  }

  int offset = i*PGSIZE;
  readFromSwapFile(proc, mem, offset, PGSIZE); 
  mappages(proc->pgdir, (char*)addr_page, PGSIZE, v2p(mem), PTE_W|PTE_U);


  if (extra_slot){ //in this case: we need to move the page we wrote to slot 15 to slot i (the one we loaded)

    char *mem_temp= kalloc();  
    if(mem_temp == 0){
      panic("memory allocation failed");
      return;
    }
    int readoffset = 15*PGSIZE;
    readFromSwapFile(proc, mem_temp, readoffset, PGSIZE); 
    
    int write_offset = i*PGSIZE;
    writeToSwapFile(proc,mem_temp,write_offset,PGSIZE);

    proc->swapped_out_pages[i] = proc->swapped_out_pages[15];         //update swapfile info
    
    kfree(mem_temp);
  }

  lcr3(v2p(proc->pgdir));             

  proc->num_of_psyc_pages++;
  proc->num_of_disk_pages--;

}

void update_accessess(){
  if (proc==0)
    return;
  
  int i;
  pte_t *pte;
  for (i = 0; i <MAX_TOTAL_PAGES; i++)
  {
      if((pte = walkpgdir(proc->pgdir, (void *) (i * PGSIZE), 0)) == 0) 
         panic("update_accessess: pte doesnt exist\n");

      if (*pte & PTE_A) { //access flag raised: increase counter and reset flag
        proc->in_use_pages[i].accesses++;
        *pte = *pte & (~PTE_A); //clear access
      }
  }
}


int is_paged_out(uint addr, uint pde){
    if ((pde & PTE_P)!=0){  //if the present flag is off
      pte_t * pt_entry = walkpgdir(proc->pgdir,(void*)addr,0);
      if (pt_entry!=0 && (*pt_entry & PTE_PG )!=0 ) //pte exists + page data is in swapfile
        return 1;
    }
    return 0;
}


void print_page_ds(){
  int i;
  if (proc==0){
      cprintf("print_ds: proc is null\n");
      return;
  }
  cprintf("\n***********PROC %s %d RAM MEMORY *****************\n",proc->name,proc->pid);
  for (i=0; i<MAX_PSYC_PAGES; i++){
    if (proc->in_use_pages[i].va!=(void*)-1)
      cprintf("index %d: address: %d, creation time: %d, accesses: %d\n",i,(int)(proc->in_use_pages[i].va),proc->in_use_pages[i].creation_time,proc->in_use_pages[i].accesses);
  }
  cprintf("num of RAM pages: %d\n",proc->num_of_psyc_pages);
 // if (proc->num_of_psyc_pages==15){
    cprintf("**************** FILE MEMORY *****************\n");
    for (i=0; i<SWAPFILESIZE; i++){
      if (proc->swapped_out_pages[i]!=(void*)-1)
        cprintf("index %d: address: %d\n",i,(int)(proc->swapped_out_pages[i]));
    }
    cprintf("num of DISK pages: %d\n",proc->num_of_disk_pages);
  //}
}