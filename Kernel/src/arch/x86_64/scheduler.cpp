#include <scheduler.h>

#include <paging.h>
#include <liballoc.h>
#include <physicalallocator.h>
#include <list.h>
#include <serial.h>
#include <idt.h>
#include <string.h>
#include <system.h>
#include <logging.h>
#include <elf.h>

#define INITIAL_HANDLE_TABLE_SIZE 0xFFFF

uintptr_t processBase;
uintptr_t processStack;
uintptr_t processEntryPoint;
uint64_t processPML4;

uint64_t kernel_stack;

bool schedulerLock = true;

extern "C" void TaskSwitch();

extern "C"
uintptr_t ReadRIP();

extern "C"
void IdleProc();

namespace Scheduler{
    bool schedulerLock = true;

    process_t* processQueueStart = 0;
    process_t* currentProcess = 0;

    uint64_t nextPID = 0;

    //List<handle_index_t>* handles;
    handle_t handles[INITIAL_HANDLE_TABLE_SIZE];
    uint32_t handleCount = 1; // We don't want null handles
    uint32_t handleTableSize = INITIAL_HANDLE_TABLE_SIZE;

    void Initialize() {
        schedulerLock = true;
        //handles = new List<handle_index_t>();
        CreateProcess((void*)IdleProc);
        processEntryPoint = currentProcess->threads[0].registers.rip;
        processStack = currentProcess->threads[0].registers.rsp;
        processBase = currentProcess->threads[0].registers.rbp;
        processPML4 = currentProcess->addressSpace->pml4Phys;
        asm("cli");
        Log::Write("OK");
        Memory::ChangeAddressSpace(currentProcess->addressSpace);
        schedulerLock = false;
        TaskSwitch();
        for(;;);
    }

    process_t* GetCurrentProcess(){ return currentProcess; }

    handle_t RegisterHandle(void* pointer){
        handle_t handle = (handle_t)handleCount++;
        handles[(uint64_t)handle] = pointer;
        return handle;
    }

    void* FindHandle(handle_t handle){
        return handles[(uint64_t)handle];
    }

    process_t* FindProcessByPID(uint64_t pid){
        process_t* proc = processQueueStart;
        if(pid == proc->pid) return proc;
            proc = proc->next;
        while (proc != processQueueStart){
            if(pid == proc->pid) return proc;
            proc = proc->next;
        }
        return NULL;
    }

    int SendMessage(message_t msg){
        process_t* proc = FindProcessByPID(msg.recieverPID);
        if(!proc) return 1; // Failed to find process with specified PID
        proc->messageQueue.add_back(msg);
        return 0; // Success
    }

    int SendMessage(process_t* proc, message_t msg){
        proc->messageQueue.add_back(msg); // Add message to queue
        return 0; // Success
    }

    message_t RecieveMessage(process_t* proc){
        if(proc->messageQueue.get_length() <= 0){
            message_t nullMsg;
            nullMsg.senderPID = 0;
            nullMsg.recieverPID = 0; // Idle process should not be asking for messages
            return nullMsg;
        }
        return proc->messageQueue.remove_at(0);
    }

    void InsertProcessIntoQueue(process_t* proc){
        if(!processQueueStart){ // If queue is empty, add the process and link to itself
            processQueueStart = proc;
            proc->next = proc;
            currentProcess = proc;
        }
        else if(processQueueStart->next){ // More than 1 process in queue?
            proc->next = processQueueStart->next;
            processQueueStart->next = proc;
        } else { // If here should only be one process in queue
            processQueueStart->next = proc;
            proc->next = processQueueStart;
        }
    }

    void RemoveProcessFromQueue(process_t* proc){
        process_t* _proc = proc->next;
        process_t* nextProc = proc->next;
        while(_proc->next && _proc != proc){
            if(_proc->next == proc){
                _proc->next = nextProc;
                break;
            }
            _proc = _proc->next;
        }
    }

    uint64_t CreateProcess(void* entry) {

        bool schedulerState = schedulerLock; // Get current value for scheduker lock
        schedulerLock = true; // Lock Scheduler

        // Create process structure
        process_t* proc = (process_t*)kmalloc(sizeof(process_t));

        memset(proc,0,sizeof(process_t));

        proc->fileDescriptors.clear();

        // Reserve 3 file descriptors for when stdin, out and err are implemented
        proc->fileDescriptors.add_back(NULL);
        proc->fileDescriptors.add_back(NULL);
        proc->fileDescriptors.add_back(NULL);

        proc->pid = nextPID++; // Set Process ID to the next availiable
        proc->priority = 1;
        proc->thread_count = 1;
        proc->state = PROCESS_STATE_ACTIVE;
        proc->thread_count = 1;

        proc->addressSpace = Memory::CreateAddressSpace();// So far this function is only used for idle task, we don't need an address space
        proc->timeSliceDefault = 1;
        proc->timeSlice = proc->timeSliceDefault;

        // Create structure for the main thread
        thread_t* thread = proc->threads;

        thread->stack = 0;
        thread->parent = proc;
        thread->priority = 1;
        thread->parent = proc;

        regs64_t* registers = &thread->registers;
        memset((uint8_t*)registers, 0, sizeof(regs64_t));
        //registers->eflags = 0x200;

        void* stack = (void*)Memory::KernelAllocate4KPages(4);
        for(int i = 0; i < 4; i++){
            Memory::KernelMapVirtualMemory4K(Memory::AllocatePhysicalMemoryBlock(),(uintptr_t)stack + PAGE_SIZE_4K * i, 1);
        }

        thread->stack = stack + 16384;
        thread->registers.rsp = (uintptr_t)thread->stack;
        thread->registers.rbp = (uintptr_t)thread->stack;
        thread->registers.rip = (uintptr_t)entry;

        InsertProcessIntoQueue(proc);

        schedulerLock = schedulerState; // Restore previous lock state

        return proc->pid;
    }

    uint64_t LoadELF(void* elf) {

        bool schedulerState = schedulerLock; // Get current value for scheduker lock
        schedulerLock = true; // Lock Scheduler

        // Create process structure
        process_t* proc = (process_t*)kmalloc(sizeof(process_t));

        memset(proc,0,sizeof(process_t));

        proc->fileDescriptors.clear();

        // Reserve 3 file descriptors for when stdin, out and err are implemented
        proc->fileDescriptors.add_back(NULL);
        proc->fileDescriptors.add_back(NULL);
        proc->fileDescriptors.add_back(NULL);

        proc->pid = nextPID++; // Set Process ID to the next availiable
        proc->priority = 1;
        proc->thread_count = 1;
        proc->state = PROCESS_STATE_ACTIVE;
        proc->thread_count = 1;

        proc->addressSpace = Memory::CreateAddressSpace();// So far this function is only used for idle task, we don't need an address space
        proc->timeSliceDefault = 10;
        proc->timeSlice = proc->timeSliceDefault;

        // Create structure for the main thread
        thread_t* thread = proc->threads;

        thread->stack = 0;
        thread->parent = proc;
        thread->priority = 1;
        thread->parent = proc;

        regs64_t* registers = &thread->registers;
        memset((uint8_t*)registers, 0, sizeof(regs64_t));
        //registers->eflags = 0x200;

        elf64_header_t elfHdr = *(elf64_header_t*)elf;
        asm("cli");

        asm volatile("mov %%rax, %%cr3" :: "a"(proc->addressSpace->pml4Phys));

        for(int i = 0; i < elfHdr.phNum; i++){
            elf64_program_header_t elfPHdr = *((elf64_program_header_t*)(elf + elfHdr.phOff + i * elfHdr.phEntrySize));

            if(elfPHdr.memSize == 0) continue;
            for(int j = 0; j < (((elfPHdr.memSize + (elfPHdr.vaddr & (PAGE_SIZE_4K-1))) / PAGE_SIZE_4K)) + 1; j++){
                uint64_t phys = Memory::AllocatePhysicalMemoryBlock();
                Memory::MapVirtualMemory4K(phys,elfPHdr.vaddr /* - (elfPHdr.vaddr & (PAGE_SIZE_4K-1))*/ + j * PAGE_SIZE_4K, 1, proc->addressSpace);
            }
        }

        Memory::MapVirtualMemory4K(Memory::AllocatePhysicalMemoryBlock(),0,1,proc->addressSpace);

        for(int i = 0; i < elfHdr.phNum; i++){
            elf64_program_header_t elfPHdr = *((elf64_program_header_t*)(elf + elfHdr.phOff + i * elfHdr.phEntrySize));

            if(elfPHdr.memSize == 0) continue;

            memset((void*)elfPHdr.vaddr,0,elfPHdr.memSize);

            memcpy((void*)elfPHdr.vaddr,(void*)(elf + elfPHdr.offset),elfPHdr.fileSize);

            Memory::ChangeAddressSpace(currentProcess->addressSpace);
        }
        
        Log::Info(elfHdr.entry);
        Log::Write(" entry");
        
        asm volatile("mov %%rax, %%cr3" :: "a"(currentProcess->addressSpace->pml4Phys));

        asm("sti");
        void* stack = (void*)Memory::KernelAllocate4KPages(4);
        for(int i = 0; i < 4; i++){
            Memory::KernelMapVirtualMemory4K(Memory::AllocatePhysicalMemoryBlock(),(uintptr_t)stack + PAGE_SIZE_4K * i, 1);
        }

        thread->stack = stack + 16384;
        thread->registers.rsp = (uintptr_t)thread->stack;
        thread->registers.rbp = (uintptr_t)thread->stack;
        thread->registers.rip = (uintptr_t)elfHdr.entry;

        InsertProcessIntoQueue(proc);

        schedulerLock = schedulerState; // Restore previous lock state

        return proc->pid;
    }

    void EndProcess(process_t* process){
        RemoveProcessFromQueue(process);/*

        for(int i = 0; i < process->fileDescriptors.get_length(); i++){
            if(process->fileDescriptors[i])
                process->fileDescriptors[i]->close(process->fileDescriptors[i]);
        }

        process->fileDescriptors.clear();

        for(int i = 0; i < process->programHeaders.get_length(); i++){
            elf32_program_header_t programHeader = process->programHeaders[i];

            for(int i = 0; i < ((programHeader.memSize / PAGE_SIZE) + 2); i++){
                uint32_t address = Memory::VirtualToPhysicalAddress(programHeader.vaddr + i * PAGE_SIZE);
                Memory::FreePhysicalMemoryBlock(address);
            }
        }
        currentProcess = process->next;
        kfree(process);

        processEntryPoint = currentProcess->threads[0].registers.eip;
        processStack = currentProcess->threads[0].registers.esp;
        processBase = currentProcess->threads[0].registers.ebp;
        processPageDirectory = currentProcess->pageDirectory.page_directory_phys;
        TaskSwitch();*/
    }

    void Tick(){
        if(currentProcess->timeSlice > 0) {
            currentProcess->timeSlice--;
            return;
        }
        if(schedulerLock) return;

        currentProcess->timeSlice = currentProcess->timeSliceDefault;

        uint64_t currentRIP = ReadRIP();

        if(currentRIP == 0xFFFFFFFF8000BEEF) {
            return;
        }

        uint64_t currentRSP;
        uint64_t currentRBP;
        asm volatile ("mov %%rsp, %0" : "=r" (currentRSP));
        asm volatile ("mov %%rbp, %0" : "=r" (currentRBP));

        currentProcess->threads[0].registers.rsp = currentRSP;
        currentProcess->threads[0].registers.rip = currentRIP;
        currentProcess->threads[0].registers.rbp = currentRBP;

        currentProcess = currentProcess->next;

        processEntryPoint = currentProcess->threads[0].registers.rip;
        processStack = currentProcess->threads[0].registers.rsp;
        processBase = currentProcess->threads[0].registers.rbp;
        processPML4 = currentProcess->addressSpace->pml4Phys;

        asm("cli");
        
        TaskSwitch();
        
        for(;;);
    }
}