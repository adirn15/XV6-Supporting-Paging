#include "types.h"
#include "stat.h"
#include "user.h"
#include "syscall.h"

#define PGSIZE 4096
#define DEBUG 0

void printline();

int
main(int argc, char *argv[]){

#ifdef LIFO
	printf(1,"########################## LIFO TEST ###########################\n\n\n");
#elif SCFIFO
    printf(1,"########################## SCFIFO TEST ###########################\n\n\n");
#elif LAP
    printf(1,"########################## LAP TEST ###########################\n\n\n");
#endif


#ifdef LIFO
    int i;
	char *arr[30];
	char input[5];
	
	gets(input, 5);

	for (i = 0; i < 11; ++i) {
		arr[i] = sbrk(PGSIZE);
		printf(1, "added page %d\n",i+4);
	}
	printline();
	printf(1, "check that RAM space is full (pages 4 to 14)\n");
	printline();
	gets(input, 5);

	for (i=11; i<26; i++){
		arr[i] = sbrk(PGSIZE);
		printf(1, "added page %d\n",i+4);
	}
	printline();
	printf(1, "check that DISK space is also full (pages 15 to 29), and 15 page outs\n");
	printline();
	gets(input, 5);

	arr[11][1]='x';

	printline();
	printf(1, "check for 1 page fault, page 14 should be replaced with page 15, 16 page outs\n");
	printline();
	gets(input, 5);

	arr[12][1]='x';

	printline();
	printf(1, "check for a new page fault- 2 in total, page 15 should be replaced with page 16\n");
	printline();
	gets(input, 5);

	sbrk(-13*PGSIZE);

	printline();
	printf(1, "check: only 2 pages on DISK after deallocation\n");
	printline();
	gets(input, 5);
	printf(1,"forking... please wait\n");
	if (fork() == 0){
		printf(1, "Created a child process %d\n",getpid());
		printline();
		printf(1, "check: child pages should be identical to father\n");
		printline();
		gets(input, 5);

		arr[11][1]='x';
		printline();
		printf(1, "check: page fault for child\n");
		printline();
		gets(input, 5);
	

		int j;
		for (j=0; j<10; j++){
			arr[j][1]='x';
		}

		printline();
		printf(1, "check child: no page faults should occur. accessed pages 4 to 14 which are in RAM\n");
		printline();
		gets(input, 5);

		printf(1, "child exiting\n");
		exit();

	}
	else {
		wait();
		printline();
		printf(1, "after pressing the next key the process should allocate 13 pages and then panic: too many pages allocated\n");
		printline();
		gets(input, 10);
		arr[28]=sbrk(14*PGSIZE);
		printf(1, "END OF TEST\n");

	}


#elif SCFIFO
    int i;
	char *arr[30];
	char input[5];
	
	gets(input, 5);

	for (i = 0; i < 12; ++i) {
		arr[i] = sbrk(PGSIZE);
		printf(1, "added page %d\n",i+4);
	}
	printline();
	printf(1, "check that RAM space is full (pages 4 to 14)\n");
	printline();
	gets(input, 5);

	arr[12] = sbrk(PGSIZE);
	arr[13] = sbrk(PGSIZE);
	arr[14] = sbrk(PGSIZE);
	
	printline();
	printf(1, "We created 3 more pages. check the pages 1,3,4 are taken out in a fifo matter (0,2 have been accessed)\n");
	printline();
	gets(input, 10);
	

	int j;
	for (j=2; j<5; j=j+2){ //3+j is the real index in RAM: 5,7
		arr[j][1]='x';
	}

	arr[1][1] = 'x';
	arr[0][1] = 'x';
	printline();
	printf(1, "We accessed pages 5,7 and then accessed the 2 pages we just moved to DISK. check for 2 page faults and that 5,7 are still in RAM\n");
	printline();
	gets(input, 10);
	
	printf(1,"forking, please wait...\n");
	

	if (fork() == 0) {
		printf(1, "created a child process %d\n",getpid());
		printf(1, "check: child pages should be identical to father, and 0 in DISK\n");
		gets(input, 10);

		arr[3][0] = 'x';
		printf(1, "check: 1 page fault in child process (accessed page 6 which was just moved to DISK)\n");
		gets(input, 10);
		exit();
	}
	else {
		wait();

		sbrk(-15 * PGSIZE);
		printf(1, "check: father has only 3 pages after deallocation\n");
		gets(input, 10);
    	printf(1, "END OF TEST\n");

	}


	#elif LAP

	int i;
	char *arr[14];
	char input[10];
	gets(input, 5);

	for (i = 0; i < 11; ++i) {
		arr[i] = sbrk(PGSIZE);
		printf(1, "created page %d\n",i+3);
		sleep(2);
	}

	printline();
	printf(1, "check that RAM space is full\n");
    printline();

	gets(input, 10);

	//accesss all array slots except arr[0]
	printf(1, "0-3 are initial pages, printing values from pages 4-13 to increase accesses (not page 14):\n");	
    for (int i=0; i<10; i++){
    	arr[i][1]='s';
    	printf(1,"%d ",arr[i][1]);
    	sleep(2);
    }

	printline();
    printf(1,"\n\ncheck: all new pages we allocated should have 1 access except for INDEX 14\n");
	printline();
	gets(input, 10);

	//index 2 in RAM is never used 
	arr[11] = sbrk(PGSIZE);

	printline();
	printf(1, "check for 1 page outs: one of the initial pages from program load\n");
	printline();	
	gets(input, 10);

	//GIVE THE NEW PAGE SOME ACCESS SO IT WONT BE OUT
   	printf(1, "printing values from the new page 16 to increase accesses so it wont go back to disk:\n");	
    for (int i=1; i<10; i++){
    	arr[11][1]='s';
    	printf(1,"%d ",arr[11][1]);
    	sleep(2);
    }
	
	arr[12] = sbrk(PGSIZE);
   	printline();	   	 
	printf(1, "check 2 page outs: index 14 in RAM replaced because it has 0 accesses\n");
	printline();		
	gets(input, 10);

   	
    arr[10][1]='x';
    printline();	
	printf(1, "check page fault: index 14 in RAM replaced again after accessing page 15\n");
	printline();	
	gets(input, 10);


	if (fork() == 0) {
		printf(1, "created a child process %d\n",getpid());
		printf(1, "check: child pages should be identical to father\n");
		gets(input, 10);

		arr[12][0] = 'x';
		printf(1, "check: 1 page fault for child\n");
		gets(input, 10);

		exit();
	}
	else {
		wait();

		sbrk(-13 * PGSIZE);
		printf(1, "check: father has only 3 pages after deallocation\n");
		gets(input, 10);
		printf(1,"END OF TEST\n");
	}


	#else //none
	char* arr[50];
	int i = 50;
	printf(1, "None: no page faults should occur\n");
	for (i = 0; i < 50; i++) {
		arr[i] = sbrk(PGSIZE);
		printf(1, "arr[%d]=0x%x\n", i, arr[i]);
	}
	printf(1,"TEST IS DONE\n");
	#endif
	exit();
}



void printline(){
	printf(1,"\n\n********************************************************************\n\n");
}
