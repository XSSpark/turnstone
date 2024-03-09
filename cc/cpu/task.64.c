/**
 * @file task.64.c
 * @brief cpu task methods
 *
 * This work is licensed under TURNSTONE OS Public License.
 * Please read and understand latest version of Licence.
 */
#include <apic.h>
#include <cpu.h>
#include <cpu/interrupt.h>
#include <cpu/task.h>
#include <memory/paging.h>
#include <memory/frame.h>
#include <list.h>
#include <time/timer.h>
#include <logging.h>
#include <systeminfo.h>
#include <linker.h>
#include <utils.h>
#include <map.h>
#include <stdbufs.h>
#include <hypervisor/hypervisor_vmxops.h>
#include <hypervisor/hypervisor_vm.h>
#include <strings.h>

MODULE("turnstone.kernel.cpu.task");

void video_text_print(const char_t* str);

typedef task_t * (*memory_current_task_getter_f)(void);
void memory_set_current_task_getter(memory_current_task_getter_f getter);

typedef task_t * (*lock_current_task_getter_f)(void);
extern lock_current_task_getter_f lock_get_current_task_getter;

typedef void (*lock_task_yielder_f)(void);
extern lock_task_yielder_f lock_task_yielder;

typedef buffer_t * (*stdbuf_task_buffer_getter_f)(void);
stdbuf_task_buffer_getter_f stdbufs_task_get_input_buffer = NULL;
stdbuf_task_buffer_getter_f stdbufs_task_get_output_buffer = NULL;
stdbuf_task_buffer_getter_f stdbufs_task_get_error_buffer = NULL;

typedef void (*future_task_wait_toggler_f)(uint64_t task_id);
extern future_task_wait_toggler_f future_task_wait_toggler_func;

uint64_t task_next_task_id = 0;

volatile task_t** current_tasks = NULL;
volatile task_t** kernel_idle_tasks = NULL;

volatile boolean_t* task_switch_paramters_need_eoi = false;
volatile boolean_t* task_switch_paramters_need_sti = false;

volatile boolean_t task_tasking_initialized = false;

list_t* task_queue = NULL;
list_t* task_cleaner_queue = NULL;
map_t task_map = NULL;
uint32_t task_mxcsr_mask = 0;

extern int8_t kmain64(void);

int8_t                                          task_task_switch_isr(interrupt_frame_ext_t* frame);
__attribute__((naked, no_stack_protector)) void task_save_registers(task_t* task);
__attribute__((naked, no_stack_protector)) void task_load_registers(task_t* task);
void                                            task_cleanup(void);
task_t*                                         task_find_next_task(uint32_t apic_id);

extern buffer_t* stdbufs_default_input_buffer;
extern buffer_t* stdbufs_default_output_buffer;
extern buffer_t* stdbufs_default_error_buffer;

void   task_idle_task(void);
int8_t task_create_idle_task(memory_heap_t* heap);

void task_toggle_wait_for_future(uint64_t task_id);

task_t* task_get_current_task(void){
    if(!current_tasks) {
        return NULL;
    }

    uint32_t apic_id = apic_get_local_apic_id();

    boolean_t old_value = cpu_cli();

    volatile task_t* current_task = current_tasks[apic_id];

    if(!old_value) {
        cpu_sti();
    }

    return (task_t*)current_task;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
int8_t task_init_tasking_ext(memory_heap_t* heap) {
    PRINTLOG(TASKING, LOG_INFO, "tasking system initialization started");

    uint64_t rsp = 0;

    __asm__ __volatile__ ("mov %%rsp, %0\n" : : "m" (rsp));

    descriptor_gdt_t* gdts = (descriptor_gdt_t*)GDT_REGISTER->base;

    descriptor_tss_t* d_tss = (descriptor_tss_t*)&gdts[3];

    size_t tmp_selector = (size_t)d_tss - (size_t)gdts;
    uint16_t tss_selector = (uint16_t)tmp_selector;

    PRINTLOG(TASKING, LOG_TRACE, "tss selector 0x%x",  tss_selector);



    program_header_t* kernel = (program_header_t*)SYSTEM_INFO->program_header_virtual_start;
    uint64_t stack_size = kernel->program_stack_size;
    uint64_t stack_top = kernel->program_stack_virtual_address;


    uint64_t stack_bottom = stack_top - 9 * stack_size;

    uint64_t frame_count = 9 * stack_size / FRAME_SIZE;

    frame_t* stack_frames = NULL;

    if(KERNEL_FRAME_ALLOCATOR->allocate_frame_by_count(KERNEL_FRAME_ALLOCATOR, frame_count, FRAME_ALLOCATION_TYPE_RESERVED | FRAME_ALLOCATION_TYPE_BLOCK, &stack_frames, NULL) != 0) {
        PRINTLOG(TASKING, LOG_FATAL, "cannot allocate stack frames of count 0x%llx", frame_count);

        return -1;
    }

    tss_t* tss = memory_malloc_ext(heap, sizeof(tss_t), 0x1000);

    if(tss == NULL) {
        PRINTLOG(TASKING, LOG_FATAL, "cannot allocate memory for tss");

        return -1;
    }

    memory_paging_add_va_for_frame(stack_bottom, stack_frames, MEMORY_PAGING_PAGE_TYPE_NOEXEC);

    PRINTLOG(TASKING, LOG_TRACE, "for tasking frames 0x%llx with count 0x%llx mapped to 0x%llx",  stack_frames->frame_address, stack_frames->frame_count, stack_bottom);

    tss->ist7 = stack_bottom + stack_size;
    tss->ist6 = tss->ist7  + stack_size;
    tss->ist5 = tss->ist6  + stack_size;
    tss->ist4 = tss->ist5  + stack_size;
    tss->ist3 = tss->ist4  + stack_size;
    tss->ist2 = tss->ist3  + stack_size;
    tss->ist1 = tss->ist2  + stack_size;
    tss->rsp2 = tss->ist1  + stack_size;
    tss->rsp1 = tss->rsp2  + stack_size;
    tss->rsp0 = rsp;

    task_queue = list_create_queue_with_heap(heap);
    task_cleaner_queue = list_create_queue_with_heap(heap);

    interrupt_irq_set_handler(0x60, &task_task_switch_isr);

    task_t* kernel_task = memory_malloc_ext(heap, sizeof(task_t), 0x0);

    if(kernel_task == NULL) {
        PRINTLOG(TASKING, LOG_FATAL, "cannot allocate memory for kernel task");

        return -1;
    }

    kernel_task->creator_heap = heap;
    kernel_task->heap = heap;
    kernel_task->task_id = TASK_KERNEL_TASK_ID;
    kernel_task->state = TASK_STATE_CREATED;
    kernel_task->entry_point = kmain64;
    kernel_task->page_table = memory_paging_get_table();
    kernel_task->fx_registers = memory_malloc_ext(heap, sizeof(uint8_t) * 512, 0x10);

    if(kernel_task->fx_registers == NULL) {
        PRINTLOG(TASKING, LOG_FATAL, "cannot allocate memory for kernel task fx registers");

        return -1;
    }

    kernel_task->stack = (void*)(stack_top + stack_size);
    kernel_task->stack_size = stack_size;
    kernel_task->task_name = "kernel";
    kernel_task->input_buffer = stdbufs_default_input_buffer;
    kernel_task->output_buffer = stdbufs_default_output_buffer;
    kernel_task->error_buffer = stdbufs_default_error_buffer;

    // get mxcsr by fxsave
    asm volatile ("mov %0, %%rax\nfxsave (%%rax)\n" : "=m" (kernel_task->fx_registers) : : "rax", "memory");

    task_mxcsr_mask = *(uint32_t*)&kernel_task->fx_registers[28];

    PRINTLOG(TASKING, LOG_INFO, "mxcsr mask 0x%x", task_mxcsr_mask);

    task_next_task_id = kernel_task->task_id + 1;

    task_map = map_integer();

    if(task_map == NULL) {
        PRINTLOG(TASKING, LOG_FATAL, "cannot allocate task map");

        return -1;
    }

    map_insert(task_map, (void*)kernel_task->task_id, kernel_task);

    uint32_t tss_limit = sizeof(tss_t) - 1;
    DESCRIPTOR_BUILD_TSS_SEG(d_tss, (size_t)tss, tss_limit, DPL_KERNEL);

    PRINTLOG(TASKING, LOG_TRACE, "task register loading with tss 0x%p limit 0x%x", tss, tss_limit);

    __asm__ __volatile__ (
        "cli\n"
        "ltr %0\n"
        "sti\n"
        : : "r" (tss_selector)
        );

    interrupt_redirect_main_interrupts(7);

    uint32_t cpu_count = apic_get_ap_count() + 1;

    PRINTLOG(TASKING, LOG_INFO, "cpu count %d", cpu_count);

    current_tasks = memory_malloc_ext(heap, sizeof(task_t*) * cpu_count, 0x0);

    if(current_tasks == NULL) {
        PRINTLOG(TASKING, LOG_FATAL, "cannot allocate memory for current tasks");

        return -1;
    }

    kernel_idle_tasks = memory_malloc_ext(heap, sizeof(task_t*) * cpu_count, 0x0);

    if(kernel_idle_tasks == NULL) {
        PRINTLOG(TASKING, LOG_FATAL, "cannot allocate memory for kernel idle tasks");

        return -1;
    }

    task_switch_paramters_need_eoi = memory_malloc_ext(heap, sizeof(boolean_t) * cpu_count, 0x0);

    if(task_switch_paramters_need_eoi == NULL) {
        PRINTLOG(TASKING, LOG_FATAL, "cannot allocate memory for task switch parameters need eoi");

        return -1;
    }

    task_switch_paramters_need_sti = memory_malloc_ext(heap, sizeof(boolean_t) * cpu_count, 0x0);

    if(task_switch_paramters_need_sti == NULL) {
        PRINTLOG(TASKING, LOG_FATAL, "cannot allocate memory for task switch parameters need sti");

        return -1;
    }

    uint32_t apic_id = apic_get_local_apic_id();

    current_tasks[apic_id] = kernel_task;
    kernel_idle_tasks[apic_id] = kernel_task;
/*
    if(task_create_idle_task(heap) != 0) {
        PRINTLOG(TASKING, LOG_FATAL, "cannot create idle task");

        return -1;
    }
 */

    PRINTLOG(TASKING, LOG_INFO, "tasking system initialization ended, kernel task address 0x%p lapic id %d", kernel_task, apic_id);

    memory_set_current_task_getter(&task_get_current_task);

    lock_get_current_task_getter = &task_get_current_task;
    lock_task_yielder = &task_yield;

    stdbufs_task_get_input_buffer = &task_get_input_buffer;
    stdbufs_task_get_output_buffer = &task_get_output_buffer;
    stdbufs_task_get_error_buffer = &task_get_error_buffer;

    future_task_wait_toggler_func = &task_toggle_wait_for_future;

    task_tasking_initialized = true;

    return 0;
}
#pragma GCC diagnostic pop

__attribute__((naked, no_stack_protector)) void task_save_registers(task_t* task) {
    __asm__ __volatile__ (
        "mov %%rax, %0\n"
        "mov %%rbx, %1\n"
        "mov %%rcx, %2\n"
        "mov %%rdx, %3\n"
        "mov %%r8,  %4\n"
        "mov %%r9,  %5\n"
        "mov %%r10, %6\n"
        "mov %%r11, %7\n"
        "mov %%r12, %8\n"
        "mov %%r13, %9\n"
        "mov %%r14, %10\n"
        "mov %%r15, %11\n"
        "mov %%rdi, %12\n"
        "mov %%rsi, %13\n"
        "mov %%rbp, %14\n"
        "push %%rbx\n"
        "mov %15, %%rbx\n"
        "fxsave (%%rbx)\n"
        "pop %%rbx\n"
        "push %%rax\n"
        "pushfq\n"
        "mov (%%rsp), %%rax\n"
        "mov %%rax, %16\n"
        "popfq\n"
        "pop %%rax\n"
        "mov %%rsp, %17\n"
        "retq\n"
        : :
        "m" (task->rax),
        "m" (task->rbx),
        "m" (task->rcx),
        "m" (task->rdx),
        "m" (task->r8),
        "m" (task->r9),
        "m" (task->r10),
        "m" (task->r11),
        "m" (task->r12),
        "m" (task->r13),
        "m" (task->r14),
        "m" (task->r15),
        "m" (task->rdi),
        "m" (task->rsi),
        "m" (task->rbp),
        "m" (task->fx_registers),
        "m" (task->rflags),
        "m" (task->rsp)
        );
}

__attribute__((naked, no_stack_protector)) void task_load_registers(task_t* task) {
    __asm__ __volatile__ (
        "mov %0,  %%rax\n"
        "mov %1,  %%rbx\n"
        "mov %2,  %%rcx\n"
        "mov %3,  %%rdx\n"
        "mov %4,  %%r8\n"
        "mov %5,  %%r9\n"
        "mov %6,  %%r10\n"
        "mov %7,  %%r11\n"
        "mov %8,  %%r12\n"
        "mov %9,  %%r13\n"
        "mov %10, %%r14\n"
        "mov %11, %%r15\n"
        "mov %13, %%rsi\n"
        "mov %14, %%rbp\n"
        "push %%rbx\n"
        "mov %15, %%rbx\n"
        "fxrstor (%%rbx)\n"
        "pop %%rbx\n"
        "push %%rax\n"
        "mov %16, %%rax\n"
        "push %%rax\n"
        "popfq\n"
        "pop %%rax\n"
        "mov %17, %%rsp\n"
        "mov %12, %%rdi\n"
        "retq\n"
        : :
        "m" (task->rax),
        "m" (task->rbx),
        "m" (task->rcx),
        "m" (task->rdx),
        "m" (task->r8),
        "m" (task->r9),
        "m" (task->r10),
        "m" (task->r11),
        "m" (task->r12),
        "m" (task->r13),
        "m" (task->r14),
        "m" (task->r15),
        "m" (task->rdi),
        "m" (task->rsi),
        "m" (task->rbp),
        "m" (task->fx_registers),
        "m" (task->rflags),
        "m" (task->rsp)
        );
}

static void task_cleanup_task(task_t* task) {
    if(task->vm) {
        hypervisor_vm_destroy(task->vm);
    }

    memory_free_ext(task->heap, (void*)task->task_name);

    map_delete(task_map, (void*)task->task_id);

    uint64_t stack_va = (uint64_t)task->stack;
    uint64_t stack_fa = MEMORY_PAGING_GET_FA_FOR_RESERVED_VA(stack_va);

    uint64_t stack_size = task->stack_size;
    uint64_t stack_frames_cnt = stack_size / FRAME_SIZE;

    memory_memclean(task->stack, stack_size);

    frame_t stack_frames = {stack_fa, stack_frames_cnt, FRAME_TYPE_USED, FRAME_ALLOCATION_TYPE_USED | FRAME_ALLOCATION_TYPE_BLOCK};

    if(memory_paging_delete_va_for_frame_ext(task->page_table, stack_va, &stack_frames) != 0 ) {
        PRINTLOG(TASKING, LOG_ERROR, "cannot remove pages for stack at va 0x%llx", stack_va);

        cpu_hlt();
    }

    if(KERNEL_FRAME_ALLOCATOR->release_frame(KERNEL_FRAME_ALLOCATOR, &stack_frames) != 0) {
        PRINTLOG(TASKING, LOG_ERROR, "cannot release stack with frames at 0x%llx with count 0x%llx", stack_fa, stack_frames_cnt);

        cpu_hlt();
    }

    uint64_t heap_va = (uint64_t)task->heap;
    uint64_t heap_fa = MEMORY_PAGING_GET_FA_FOR_RESERVED_VA(heap_va);

    uint64_t heap_size = task->heap_size;
    uint64_t heap_frames_cnt = heap_size / FRAME_SIZE;

    memory_memclean(task->heap, heap_size);

    frame_t heap_frames = {heap_fa, heap_frames_cnt, FRAME_TYPE_USED, FRAME_ALLOCATION_TYPE_USED | FRAME_ALLOCATION_TYPE_BLOCK};

    if(memory_paging_delete_va_for_frame_ext(task->page_table, heap_va, &heap_frames) != 0 ) {
        PRINTLOG(TASKING, LOG_ERROR, "cannot remove pages for heap at va 0x%llx", heap_va);

        if(task->page_table) {
            PRINTLOG(TASKING, LOG_ERROR, "page table 0x%p", task->page_table->page_table);
        }

        cpu_hlt();
    }

    if(KERNEL_FRAME_ALLOCATOR->release_frame(KERNEL_FRAME_ALLOCATOR, &heap_frames) != 0) {
        PRINTLOG(TASKING, LOG_ERROR, "cannot release heap with frames at 0x%llx with count 0x%llx", stack_fa, heap_frames_cnt);

        cpu_hlt();
    }

    memory_free_ext(task->creator_heap, task->fx_registers);
    memory_free_ext(task->creator_heap, task);
}

void task_cleanup(void){
    while(list_size(task_cleaner_queue)) {
        task_t* task = (task_t*)list_queue_pop(task_cleaner_queue);
        task_cleanup_task(task);
    }
}

boolean_t task_idle_check_need_yield(void) {
    boolean_t need_yield = false;

    for(uint64_t i = 0; i < list_size(task_queue); i++) {
        task_t* t = (task_t*)list_get_data_at_position(task_queue, i);

        if(!t) {
            continue;
        }

        if(t->state == TASK_STATE_ENDED) {
            continue;
        }

        if(t->message_waiting) {
            if(t->interruptible && t->interrupt_received) {
                // t->interrupt_received = false;
                need_yield = true;
                break;
            }

            if(t->message_queues) {
                for(uint64_t q_idx = 0; q_idx < list_size(t->message_queues); q_idx++) {
                    list_t* q = (list_t*)list_get_data_at_position(t->message_queues, q_idx);

                    if(list_size(q)) {
                        // t->message_waiting = false;
                        need_yield = true;
                        break;
                    }
                }
            }
        } else if(t->sleeping) {
            if(t->wake_tick < time_timer_get_tick_count()) {
                // t->sleeping = false;
                need_yield = true;
                break;
            }
        } else if(t->wait_for_future) {
            break;
        }
    }

    return need_yield;
}

extern volatile boolean_t kmain64_completed;

task_t* task_find_next_task(uint32_t apic_id) {
    task_t* tmp_task = NULL;

    uint64_t found_index = -1;

    for(uint64_t i = 0; i < list_size(task_queue); i++) {
        task_t* t = (task_t*)list_get_data_at_position(task_queue, i);

        if(t->state == TASK_STATE_ENDED) {
            found_index = i;
            break; // trick to remove ended task from queue
        }

        if(t->wait_for_future) {
            continue;
        } else if(t->sleeping) {
            if(t->wake_tick < time_timer_get_tick_count()) {
                t->sleeping = false;
                found_index = i;
                break;
            }
        } else if(t->message_waiting) {
            if(t->interruptible) {
                if(t->interrupt_received) {
                    t->interrupt_received = false;
                    t->message_waiting = false;
                    found_index = i;
                    break;
                }
            }

            if(t->message_queues) {
                for(uint64_t q_idx = 0; q_idx < list_size(t->message_queues); q_idx++) {
                    list_t* q = (list_t*)list_get_data_at_position(t->message_queues, q_idx);

                    if(list_size(q)) {
                        t->message_waiting = false;
                        found_index = i;
                        break;
                    }
                }
            }
        } else {
            found_index = i;
            break;
        }
    }

    if(found_index != -1ULL) {
        tmp_task = (task_t*)list_delete_at_position(task_queue, found_index);

        // need check it is ended?
        if(tmp_task->state == TASK_STATE_ENDED) {
            // it is already task clear queue so find next
            list_queue_push(task_cleaner_queue, tmp_task);
            tmp_task = (task_t*)kernel_idle_tasks[apic_id]; // idle task will clean it
        }
    } else {
        tmp_task = (task_t*)kernel_idle_tasks[apic_id];
    }


    // PRINTLOG(TASKING, LOG_WARNING, "task 0x%llx selected for execution. queue size %lli", tmp_task->task_id, list_size(task_queue));

    return tmp_task;
}

void task_task_switch_set_parameters(boolean_t need_eoi, boolean_t need_sti) {
    uint32_t apic_id = apic_get_local_apic_id();

    task_switch_paramters_need_eoi[apic_id] = need_eoi;
    task_switch_paramters_need_sti[apic_id] = need_sti;
}


static __attribute__((noinline)) void task_switch_task_exit_prep(void) {
    uint32_t apic_id = apic_get_local_apic_id();

    if(task_switch_paramters_need_eoi[apic_id]) {
        apic_eoi();
    }

    if(task_switch_paramters_need_sti[apic_id]) {
        cpu_sti();
    }
}

__attribute__((no_stack_protector)) void task_switch_task(void) {
    task_t* current_task = task_get_current_task();
    uint32_t apic_id = apic_get_local_apic_id();

    if(current_task == NULL) {
        task_switch_task_exit_prep();

        return;
    }

    if(current_task->state != TASK_STATE_ENDED) {
        if((time_timer_get_tick_count() - current_task->last_tick_count) < TASK_MAX_TICK_COUNT &&
           time_timer_get_tick_count() > current_task->last_tick_count &&
           !current_task->message_waiting &&
           !current_task->sleeping) {

            task_switch_task_exit_prep();

            return;
        }
    }

    if(current_task->vmcs_physical_address) {
        if(vmclear(current_task->vmcs_physical_address) != 0) {
            PRINTLOG(TASKING, LOG_ERROR, "vmclear failed for task 0x%llx", current_task->task_id);
            return;
        }
    }

    task_save_registers(current_task);

    if(current_task->state != TASK_STATE_ENDED) {
        current_task->state = TASK_STATE_SUSPENDED;
    }

    if(!kmain64_completed || current_task->task_id != TASK_KERNEL_TASK_ID) {
        list_queue_push(task_queue, current_task);
    }

    if(current_task->task_id == TASK_KERNEL_TASK_ID && list_size(task_cleaner_queue) > 0) {
        task_cleanup();
    }

    current_task = task_find_next_task(apic_id);
    current_task->last_tick_count = time_timer_get_tick_count();
    current_task->task_switch_count++;

    current_tasks[apic_id] = current_task;
    current_task->state = TASK_STATE_RUNNING;

    if(current_task->vmcs_physical_address) {
        if(vmptrld(current_task->vmcs_physical_address) != 0) {
            PRINTLOG(TASKING, LOG_ERROR, "vmptrld failed for task 0x%llx", current_task->task_id);
        }
    }

    task_load_registers(current_task);

    task_switch_task_exit_prep();
}

void task_end_task(void) {
    task_t* current_task = task_get_current_task();

    if(current_task == NULL) {
        return;
    }

    PRINTLOG(TASKING, LOG_TRACE, "ending task 0x%lli", current_task->task_id);

    if(current_task->vmcs_physical_address) {
        if(vmclear(current_task->vmcs_physical_address) != 0) {
            PRINTLOG(TASKING, LOG_ERROR, "vmclear failed for task 0x%llx", current_task->task_id);
        }
    }

    current_task->message_waiting = false;
    current_task->interruptible = false;
    current_task->wait_for_future = false;

    current_task->state = TASK_STATE_ENDED;

    task_yield();
}

void task_kill_task(uint64_t task_id, boolean_t force) {
    task_t* task = (task_t*)map_get(task_map, (void*)task_id);

    if(task == NULL) {
        PRINTLOG(TASKING, LOG_WARNING, "task 0x%lli not found", task_id);
        return;
    }

    if(task->state == TASK_STATE_ENDED) {
        if(force) {
            task_cleanup_task(task);
        }

        return;
    }

    task->message_waiting = false;
    task->interruptible = false;
    task->wait_for_future = false;

    task->state = TASK_STATE_ENDED;


    PRINTLOG(TASKING, LOG_INFO, "task 0x%lli will be ended", task->task_id);
}


void task_add_message_queue(list_t* queue){
    task_t* current_task = task_get_current_task();

    if(!current_task) {
        return;
    }

    if(current_task->message_queues == NULL) {
        current_task->message_queues = list_create_list();
    }

    list_list_insert(current_task->message_queues, queue);
}

list_t* task_get_message_queue(uint64_t task_id, uint64_t queue_number) {
    const task_t* task = map_get(task_map, (void*)task_id);

    if(task == NULL) {
        return NULL;
    }

    if(task->message_queues == NULL) {
        return NULL;
    }

    return (list_t*)list_get_data_at_position(task->message_queues, queue_number);
}

void task_set_message_waiting(void){
    task_t* current_task = task_get_current_task();

    if(current_task) {
        current_task->message_waiting = 1;
    }
}

uint64_t task_create_task(memory_heap_t* heap, uint64_t heap_size, uint64_t stack_size, void* entry_point, uint64_t args_cnt, void** args, const char_t* task_name) {

    task_t* new_task = memory_malloc_ext(heap, sizeof(task_t), 0x0);

    if(new_task == NULL) {
        return -1;
    }

    new_task->creator_heap = heap;

    uint8_t* fx_registers = memory_malloc_ext(heap, sizeof(uint8_t) * 512, 0x10);

    if(fx_registers == NULL) {
        memory_free_ext(heap, new_task);

        return -1;
    }


    frame_t* stack_frames;
    uint64_t stack_frames_cnt = (stack_size + FRAME_SIZE - 1) / FRAME_SIZE;
    stack_size = stack_frames_cnt * FRAME_SIZE;

    if(KERNEL_FRAME_ALLOCATOR->allocate_frame_by_count(KERNEL_FRAME_ALLOCATOR, stack_frames_cnt, FRAME_ALLOCATION_TYPE_USED | FRAME_ALLOCATION_TYPE_BLOCK, &stack_frames, NULL) != 0) {
        PRINTLOG(TASKING, LOG_ERROR, "cannot allocate stack with frame count 0x%llx", stack_frames_cnt);
        memory_free_ext(heap, new_task);
        memory_free_ext(heap, fx_registers);

        return -1;
    }

    frame_t* heap_frames;
    uint64_t heap_frames_cnt = (heap_size + FRAME_SIZE - 1) / FRAME_SIZE;
    heap_size = heap_frames_cnt * FRAME_SIZE;

    if(KERNEL_FRAME_ALLOCATOR->allocate_frame_by_count(KERNEL_FRAME_ALLOCATOR, heap_frames_cnt, FRAME_ALLOCATION_TYPE_USED | FRAME_ALLOCATION_TYPE_BLOCK, &heap_frames, NULL) != 0) {
        PRINTLOG(TASKING, LOG_ERROR, "cannot allocate heap with frame count 0x%llx", heap_frames_cnt);

        if(KERNEL_FRAME_ALLOCATOR->release_frame(KERNEL_FRAME_ALLOCATOR, stack_frames) != 0) {
            PRINTLOG(TASKING, LOG_ERROR, "cannot release stack with frames at 0x%llx with count 0x%llx", stack_frames->frame_address, stack_frames->frame_count);

            cpu_hlt();
        }

        memory_free_ext(heap, new_task);
        memory_free_ext(heap, fx_registers);

        return -1;
    }

    uint64_t stack_va = MEMORY_PAGING_GET_VA_FOR_RESERVED_FA(stack_frames->frame_address);

    if(memory_paging_add_va_for_frame(stack_va, stack_frames, MEMORY_PAGING_PAGE_TYPE_NOEXEC) != 0) {
        PRINTLOG(TASKING, LOG_ERROR, "cannot add stack va 0x%llx for frame at 0x%llx with count 0x%llx", stack_va, stack_frames->frame_address, stack_frames->frame_count);

        cpu_hlt();
    }

    memory_memclean((void*)stack_va, stack_size);

    uint64_t heap_va = MEMORY_PAGING_GET_VA_FOR_RESERVED_FA(heap_frames->frame_address);

    if(memory_paging_add_va_for_frame(heap_va, heap_frames, MEMORY_PAGING_PAGE_TYPE_NOEXEC) != 0) {
        PRINTLOG(TASKING, LOG_ERROR, "cannot add heap va 0x%llx for frame at 0x%llx with count 0x%llx", heap_va, heap_frames->frame_address, heap_frames->frame_count);

        cpu_hlt();
    }

    memory_heap_t* task_heap = NULL;

    if(heap_size > (16 << 20)) {
        task_heap = memory_create_heap_hash(heap_va, heap_va + heap_size);
    } else {
        task_heap = memory_create_heap_simple(heap_va, heap_va + heap_size);
    }

    boolean_t old_int_status = cpu_cli();

    uint64_t new_task_id = task_next_task_id++;

    if(!old_int_status) {
        cpu_sti();
    }

    task_heap->task_id = new_task_id;

    new_task->heap = task_heap;
    new_task->heap_size = heap_size;
    new_task->task_id = new_task_id;
    new_task->state = TASK_STATE_CREATED;
    new_task->entry_point = entry_point;
    new_task->page_table = memory_paging_get_table();
    new_task->fx_registers = fx_registers;
    new_task->rflags = 0x202;
    new_task->stack_size = stack_size;
    new_task->stack = (void*)stack_va;
    new_task->task_name = strdup_at_heap(task_heap, task_name);

    *(uint16_t*)&new_task->fx_registers[0] = 0x37F;
    *(uint32_t*)&new_task->fx_registers[24] = 0x1F80 & task_mxcsr_mask;

    uint64_t rbp = (uint64_t)new_task->stack;
    rbp += stack_size - 16;
    new_task->rbp = rbp;
    new_task->rsp = rbp - 32; // 24 is for last return address, entry point and end task

    new_task->rdi = args_cnt;
    new_task->rsi = (uint64_t)args;

    uint64_t* stack = (uint64_t*)rbp;
    stack[-1] = (uint64_t)task_end_task;
    stack[-2] = (uint64_t)entry_point;
    // stack[-3] = (uint64_t)task_switch_task_exit_prep; // do we need this?
    stack[-3] = (uint64_t)cpu_sti; // FIXME: force sti for now
    stack[-4] = (uint64_t)apic_eoi; // FIXME: force eoi for now may be we lose some interrupts


    new_task->input_buffer = buffer_create_with_heap(task_heap, 0x1000);
    new_task->output_buffer = buffer_create_with_heap(task_heap, 0x1000);
    new_task->error_buffer = buffer_create_with_heap(task_heap, 0x1000);

    PRINTLOG(TASKING, LOG_INFO, "scheduling new task %s 0x%llx 0x%p stack at 0x%llx-0x%llx heap at 0x%p[0x%llx]",
             new_task->task_name, new_task->task_id, new_task, new_task->rsp, new_task->rbp, new_task->heap, new_task->heap_size);

    old_int_status = cpu_cli();

    list_stack_push(task_queue, new_task);
    map_insert(task_map, (void*)new_task->task_id, new_task);

    if(!old_int_status) {
        cpu_sti();
    }

    PRINTLOG(TASKING, LOG_INFO, "task %s 0x%llx added to task queue", new_task->task_name, new_task->task_id);

    return new_task->task_id;
}

void task_idle_task(void) {
    while(true) {
        asm volatile ("hlt\n");
    }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
int8_t task_create_idle_task(memory_heap_t* heap) {
    task_t* new_task = memory_malloc_ext(heap, sizeof(task_t), 0x0);

    if(new_task == NULL) {
        return -1;
    }

    new_task->creator_heap = heap;

    uint8_t* fx_registers = memory_malloc_ext(heap, sizeof(uint8_t) * 512, 0x10);

    if(fx_registers == NULL) {
        memory_free_ext(heap, new_task);

        return -1;
    }

    uint64_t stack_size = 0x1000;


    frame_t* stack_frames;
    uint64_t stack_frames_cnt = (stack_size + FRAME_SIZE - 1) / FRAME_SIZE;
    stack_size = stack_frames_cnt * FRAME_SIZE;

    if(KERNEL_FRAME_ALLOCATOR->allocate_frame_by_count(KERNEL_FRAME_ALLOCATOR, stack_frames_cnt, FRAME_ALLOCATION_TYPE_USED | FRAME_ALLOCATION_TYPE_BLOCK, &stack_frames, NULL) != 0) {
        PRINTLOG(TASKING, LOG_ERROR, "cannot allocate stack with frame count 0x%llx", stack_frames_cnt);
        memory_free_ext(heap, new_task);
        memory_free_ext(heap, fx_registers);

        return -1;
    }

    uint64_t stack_va = MEMORY_PAGING_GET_VA_FOR_RESERVED_FA(stack_frames->frame_address);

    if(memory_paging_add_va_for_frame(stack_va, stack_frames, MEMORY_PAGING_PAGE_TYPE_NOEXEC) != 0) {
        PRINTLOG(TASKING, LOG_ERROR, "cannot add stack va 0x%llx for frame at 0x%llx with count 0x%llx", stack_va, stack_frames->frame_address, stack_frames->frame_count);

        cpu_hlt();
    }

    boolean_t old_int_status = cpu_cli();

    uint64_t new_task_id = task_next_task_id++;

    if(!old_int_status) {
        cpu_sti();
    }

    new_task->heap = NULL;
    new_task->heap_size = 0;
    new_task->task_id = new_task_id;
    new_task->state = TASK_STATE_CREATED;
    new_task->entry_point = task_idle_task;
    new_task->page_table = memory_paging_get_table();
    new_task->fx_registers = fx_registers;
    new_task->rflags = 0x202;
    new_task->stack_size = stack_size;
    new_task->stack = (void*)stack_va;
    new_task->task_name = "idle";

    *(uint16_t*)&new_task->fx_registers[0] = 0x37F;
    *(uint32_t*)&new_task->fx_registers[24] = 0x1F80 & task_mxcsr_mask;

    uint64_t rbp = (uint64_t)new_task->stack;
    rbp += stack_size - 16;
    new_task->rbp = rbp;
    new_task->rsp = rbp - 32;

    uint64_t* stack = (uint64_t*)rbp;
    stack[-1] = (uint64_t)task_end_task;
    stack[-2] = (uint64_t)new_task->entry_point;
    // stack[-3] = (uint64_t)task_switch_task_exit_prep; // do we need this?
    stack[-3] = (uint64_t)cpu_sti; // FIXME: force sti for now
    stack[-4] = (uint64_t)apic_eoi; // FIXME: force eoi for now may be we lose some interrupts

    old_int_status = cpu_cli();

    list_stack_push(task_queue, new_task);
    map_insert(task_map, (void*)new_task->task_id, new_task);

    if(!old_int_status) {
        cpu_sti();
    }

    PRINTLOG(TASKING, LOG_INFO, "task[0x%p] with ep 0x%p %s 0x%llx added to task queue", new_task, new_task->entry_point, new_task->task_name, new_task->task_id);

    return 0;
}
#pragma GCC diagnostic pop

void task_yield(void) {
    if(list_size(task_queue)) { // prevent unneccessary interrupt
        // __asm__ __volatile__ ("int $0x80\n");
        cpu_cli();
        task_task_switch_set_parameters(false, true);
        task_switch_task();
    }
}

int8_t task_task_switch_isr(interrupt_frame_ext_t* frame) {
    UNUSED(frame);

    task_task_switch_set_parameters(true, false);
    task_switch_task();

    return 0;
}


uint64_t task_get_id(void) {
    uint64_t id = TASK_KERNEL_TASK_ID;

    task_t* current_task = task_get_current_task();

    if(current_task) {
        id = current_task->task_id;
    }

    return id;
}

void task_current_task_sleep(uint64_t wake_tick) {
    task_t* current_task = task_get_current_task();

    if(current_task) {
        current_task->wake_tick = wake_tick;
        current_task->sleeping = true;
        task_yield();
    }
}

void task_clear_message_waiting(uint64_t tid) {
    task_t* task = (task_t*)map_get(task_map, (void*)tid);

    if(task) {
        task->message_waiting = false;
    } else {
        PRINTLOG(TASKING, LOG_ERROR, "task not found 0x%llx", tid);
    }
}

void task_set_interrupt_received(uint64_t tid) {
    task_t* task = (task_t*)map_get(task_map, (void*)tid);

    if(task) {
        task->interrupt_received = true;
    } else {
        PRINTLOG(TASKING, LOG_ERROR, "task not found 0x%llx", tid);
    }
}

void task_set_interruptible(void) {
    task_t* current_task = task_get_current_task();

    if(current_task) {
        current_task->interruptible = true;
    }
}

void task_print_all(void) {
    iterator_t* it = map_create_iterator(task_map);

    while(it->end_of_iterator(it) != 0) {
        const task_t* task = it->get_item(it);


        uint64_t msgcount = 0;

        if(task->message_queues) {
            for(uint64_t q_idx = 0; q_idx < list_size(task->message_queues); q_idx++) {
                list_t* q = (list_t*)list_get_data_at_position(task->message_queues, q_idx);

                msgcount += list_size(q);
            }
        }

        printf("\ttask %s 0x%llx 0x%p switched 0x%llx stack at 0x%llx-0x%llx heap at 0x%p[0x%llx] stack 0x%p[0x%llx]\n",
               task->task_name, task->task_id, task, task->task_switch_count,
               task->rsp, task->rbp, task->heap, task->heap_size,
               task->stack, task->stack_size);

        printf("\t\tinterruptible %d sleeping %d message_waiting %d interrupt_received %d future waiting %d state %d\n",
               task->interruptible, task->sleeping, task->message_waiting, task->interrupt_received,
               task->wait_for_future, task->state);

        printf("\t\tmessage queues %lli messages %lli\n", list_size(task->message_queues), msgcount);

        memory_heap_stat_t stat = {0};
        memory_get_heap_stat_ext(task->heap, &stat);

        printf("\t\theap malloc 0x%llx free 0x%llx diff 0x%llx\n", stat.malloc_count, stat.free_count, stat.malloc_count - stat.free_count);

        it = it->next(it);
    }

    it->destroy(it);
}

buffer_t* task_get_task_input_buffer(uint64_t tid) {
    task_t* task = (task_t*)map_get(task_map, (void*)tid);

    if(task) {
        return task->input_buffer;
    }

    return NULL;
}

buffer_t* task_get_task_output_buffer(uint64_t tid) {
    task_t* task = (task_t*)map_get(task_map, (void*)tid);

    if(task) {
        return task->output_buffer;
    }

    return NULL;
}

buffer_t* task_get_task_error_buffer(uint64_t tid) {
    task_t* task = (task_t*)map_get(task_map, (void*)tid);

    if(task) {
        return task->error_buffer;
    }

    return NULL;
}

buffer_t* task_get_input_buffer(void) {
    buffer_t* buffer = NULL;

    task_t* current_task = task_get_current_task();

    if(current_task) {
        buffer = current_task->input_buffer;
    }

    return buffer;
}

buffer_t* task_get_output_buffer(void) {
    buffer_t* buffer = NULL;

    task_t* current_task = task_get_current_task();

    if(current_task) {
        buffer = current_task->output_buffer;
    }

    return buffer;
}

buffer_t* task_get_error_buffer(void) {
    buffer_t* buffer = NULL;

    task_t* current_task = task_get_current_task();

    if(current_task) {
        buffer = current_task->error_buffer;
    }

    return buffer;
}

int8_t task_set_input_buffer(buffer_t* buffer) {
    if(!buffer) {
        return -1;
    }

    task_t* current_task = task_get_current_task();

    if(!current_task) {
        return -1;
    }

    current_task->input_buffer = buffer;

    return 0;
}

int8_t task_set_output_buffer(buffer_t * buffer) {
    if(!buffer) {
        return -1;
    }

    task_t* current_task = task_get_current_task();

    if(!current_task) {
        return -1;
    }

    current_task->output_buffer = buffer;

    return 0;
}

int8_t task_set_error_buffer(buffer_t * buffer) {
    if(!buffer) {
        return -1;
    }

    task_t* current_task = task_get_current_task();

    if(!current_task) {
        return -1;
    }

    current_task->error_buffer = buffer;

    return 0;
}

void task_set_vmcs_physical_address(uint64_t vmcs_physical_address) {
    task_t* current_task = task_get_current_task();

    if(current_task) {
        current_task->vmcs_physical_address = vmcs_physical_address;
    }
}

uint64_t task_get_vmcs_physical_address(void) {
    uint64_t vmcs_physical_address = 0;

    task_t* current_task = task_get_current_task();

    if(current_task) {
        vmcs_physical_address = current_task->vmcs_physical_address;
    }

    return vmcs_physical_address;
}

void task_set_vm(void* vm) {
    task_t* current_task = task_get_current_task();

    if(current_task) {
        current_task->vm = vm;
    }
}

void* task_get_vm(void) {
    void* vm = NULL;

    task_t* current_task = task_get_current_task();

    if(current_task) {
        vm = current_task->vm;
    }

    return vm;
}

void task_toggle_wait_for_future(uint64_t tid) {
    if(tid == 0 || tid == TASK_KERNEL_TASK_ID) {
        return;
    }

    task_t* task = (task_t*)map_get(task_map, (void*)tid);

    if(task) {
        char_t buf[64] = {0};
        itoa_with_buffer(buf, task->task_id);
        video_text_print("task id: ");
        video_text_print(buf);
        video_text_print(" wait for future: ");
        video_text_print(!task->wait_for_future ? "true" : "false");
        video_text_print("\n");

        task->wait_for_future = !task->wait_for_future;
    }
}

void task_remove_task_after_fault(uint64_t task_id) {
    task_t* task = (task_t*)map_get(task_map, (void*)task_id);

    task->state = TASK_STATE_ENDED;

    uint32_t apic_id = apic_get_local_apic_id();

    task_t* current_task = (task_t*)kernel_idle_tasks[apic_id];
    current_task->last_tick_count = time_timer_get_tick_count();
    current_task->task_switch_count++;

    current_tasks[apic_id] = current_task;
    current_task->state = TASK_STATE_RUNNING;

    task_load_registers(current_task);

    cpu_sti();
}
