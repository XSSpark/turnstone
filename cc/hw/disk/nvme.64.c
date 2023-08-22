/**
 * @file nvme.h
 * @brief nvme interface
 *
 * This work is licensed under TURNSTONE OS Public License.
 * Please read and understand latest version of Licence.
 */

#include <driver/nvme.h>
#include <pci.h>
#include <video.h>
#include <logging.h>
#include <memory/frame.h>
#include <memory/paging.h>
#include <time/timer.h>
#include <apic.h>
#include <hashmap.h>


MODULE("turnstone.kernel.hw.drivers");

hashmap_t* nvme_disks = NULL;
hashmap_t* nvme_disk_isr_map = NULL;

int8_t   nvme_isr(interrupt_frame_t* frame, uint8_t intnum);
int8_t   nvme_identify(nvme_disk_t* nvme_disk, uint32_t cns, uint32_t nsid, uint64_t data_address);
int8_t   nvme_enable_cache(nvme_disk_t* nvme_disk);
int8_t   nvme_set_queue_count(nvme_disk_t* nvme_disk, uint16_t io_sq_count, uint16_t io_cq_count);
future_t nvme_read_write(uint64_t disk_id, uint64_t lba, uint16_t size, uint8_t* buffer, boolean_t write);


int8_t nvme_isr(interrupt_frame_t* frame, uint8_t intnum) {
    UNUSED(frame);
    PRINTLOG(NVME, LOG_DEBUG, "nvme isr %i", intnum);
    nvme_disk_t* nvme_disk = (nvme_disk_t*)hashmap_get(nvme_disk_isr_map, (void*)(uint64_t)intnum);

    if(nvme_disk == NULL) {
        PRINTLOG(NVME, LOG_ERROR, "cannot find nvme disk for isr %i", intnum);
        apic_eoi();

        return 0;
    }

    uint32_t status_code = nvme_disk->io_completion_queue[nvme_disk->io_c_queue_head].status_code;
    uint32_t status_type = nvme_disk->io_completion_queue[nvme_disk->io_c_queue_head].status_type;
    uint32_t sqhd = nvme_disk->io_completion_queue[nvme_disk->io_c_queue_head].sqhd;
    uint32_t sqid = nvme_disk->io_completion_queue[nvme_disk->io_c_queue_head].sqid;
    uint32_t cid = nvme_disk->io_completion_queue[nvme_disk->io_c_queue_head].cid;

    PRINTLOG(NVME, LOG_DEBUG, "status code %i", status_code);
    PRINTLOG(NVME, LOG_DEBUG, "status type %i", status_type);
    PRINTLOG(NVME, LOG_DEBUG, "sqhd %i", sqhd);
    PRINTLOG(NVME, LOG_DEBUG, "sqid %i", sqid);
    PRINTLOG(NVME, LOG_DEBUG, "cid %i", cid);

    lock_t lock = (lock_t)hashmap_get(nvme_disk->command_lock_map, (void*)(uint64_t)cid);

    if(lock == NULL) {
        PRINTLOG(NVME, LOG_ERROR, "cannot find lock for cid %i", cid);
    }

    lock_release(lock);

    nvme_disk->io_c_queue_head = (nvme_disk->io_c_queue_head + 1) % 64;
    *nvme_disk->io_completion_queue_head_doorbell = nvme_disk->io_c_queue_head;


    pci_msix_clear_pending_bit(nvme_disk->pci_device, nvme_disk->msix_capability, 1, true);
    apic_eoi();
    return 0;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
int8_t nvme_init(memory_heap_t* heap, linkedlist_t nvme_pci_devices) {
    PRINTLOG(NVME, LOG_INFO, "disk searching started");

    if(linkedlist_size(nvme_pci_devices) == 0) {
        PRINTLOG(NVME, LOG_WARNING, "no SATA devices");
        return 0;
    }

    nvme_disks = hashmap_integer(16);

    if(nvme_disks == NULL) {
        PRINTLOG(NVME, LOG_ERROR, "cannot allocate memory for nvme disks");

        return -1;
    }

    nvme_disk_isr_map = hashmap_integer(16);

    if(nvme_disk_isr_map == NULL) {
        PRINTLOG(NVME, LOG_ERROR, "cannot allocate memory for nvme disk isr map");

        return -1;
    }

    uint64_t disk_id = 0;

    iterator_t* iter = linkedlist_iterator_create(nvme_pci_devices);

    while(iter->end_of_iterator(iter) != 0) {
        const pci_dev_t* p = iter->get_item(iter);
        pci_generic_device_t* pci_nvme = (pci_generic_device_t*)p->pci_header;

        nvme_disk_t* nvme_disk = memory_malloc_ext(heap, sizeof(nvme_disk_t), 0);

        if(nvme_disk == NULL) {
            PRINTLOG(NVME, LOG_ERROR, "cannot allocate memory for nvme disk");

            iter->destroy(iter);

            return -1;
        }

        nvme_disk->command_lock_map = hashmap_integer(64);

        if(nvme_disk->command_lock_map == NULL) {
            PRINTLOG(NVME, LOG_ERROR, "cannot allocate memory for nvme command lock map");
            memory_free_ext(heap, nvme_disk);
            iter->destroy(iter);

            return -1;
        }

        nvme_disk->heap = heap;
        nvme_disk->disk_id = disk_id;
        nvme_disk->pci_device = pci_nvme;


        pci_capability_msi_t* msi_cap = NULL;
        pci_capability_msix_t* msix_cap = NULL;

        if(pci_nvme->common_header.status.capabilities_list) {
            pci_capability_t* pci_cap = (pci_capability_t*)(((uint8_t*)pci_nvme) + pci_nvme->capabilities_pointer);


            while(pci_cap->capability_id != 0xFF) {
                if(pci_cap->capability_id == PCI_DEVICE_CAPABILITY_MSI) {
                    msi_cap = (pci_capability_msi_t*)pci_cap;
                } if(pci_cap->capability_id == PCI_DEVICE_CAPABILITY_MSIX) {
                    msix_cap = (pci_capability_msix_t*)pci_cap;
                }else {
                    PRINTLOG(NVME, LOG_WARNING, "not implemented cap 0x%02x", pci_cap->capability_id);
                }

                if(pci_cap->next_pointer == NULL) {
                    break;
                }

                pci_cap = (pci_capability_t*)(((uint8_t*)pci_nvme) + pci_cap->next_pointer);
            }
        }

        if(msix_cap != NULL) {
            if(pci_msix_configure(pci_nvme, msix_cap) != 0) {
                PRINTLOG(VIRTIO, LOG_ERROR, "failed to configure msix");
                memory_free_ext(heap, nvme_disk);

                return -1;
            }
        } else {
            PRINTLOG(NVME, LOG_ERROR, "no msix cap");
            memory_free_ext(heap, nvme_disk);

            return -1;
        }

        nvme_disk->msix_capability = msix_cap;

        uint64_t bar_fa = pci_get_bar_address(pci_nvme, 0);

        PRINTLOG(NVME, LOG_TRACE, "frame address at bar 0x%llx", bar_fa);

        frame_t* bar_frames = KERNEL_FRAME_ALLOCATOR->get_reserved_frames_of_address(KERNEL_FRAME_ALLOCATOR, (void*)bar_fa);
        uint64_t size = pci_get_bar_size(pci_nvme, 0);
        PRINTLOG(NVME, LOG_TRACE, "bar size 0x%llx", size);
        uint64_t bar_frm_cnt = (size + FRAME_SIZE - 1) / FRAME_SIZE;
        frame_t bar_req_frm = {bar_fa, bar_frm_cnt, FRAME_TYPE_RESERVED, 0};

        uint64_t bar_va = MEMORY_PAGING_GET_VA_FOR_RESERVED_FA(bar_fa);

        if(bar_frames == NULL) {
            PRINTLOG(NVME, LOG_TRACE, "cannot find reserved frames for 0x%llx and try to reserve", bar_fa);

            if(KERNEL_FRAME_ALLOCATOR->allocate_frame(KERNEL_FRAME_ALLOCATOR, &bar_req_frm) != 0) {
                PRINTLOG(NVME, LOG_ERROR, "cannot allocate frame");
                memory_free_ext(heap, nvme_disk);

                return -1;
            }
        }

        memory_paging_add_va_for_frame(bar_va, &bar_req_frm, MEMORY_PAGING_PAGE_TYPE_NOEXEC);

        nvme_controller_registers_t* nvme_regs = (nvme_controller_registers_t*)bar_va;

        PRINTLOG(NVME, LOG_DEBUG, "nvme controller %lli ready? %i has msi? %i has_msix? %i", disk_id, nvme_regs->status.ready, msi_cap != NULL, msix_cap != NULL);
        PRINTLOG(NVME, LOG_DEBUG, "nvme version %i.%i.%i", nvme_regs->version.major, nvme_regs->version.minor, nvme_regs->version.reserved_or_ter);
        PRINTLOG(NVME, LOG_DEBUG, "nvme queue size %i", nvme_regs->capabilities.fields.mqes + 1);

        frame_t* queue_frames = NULL;

        if(KERNEL_FRAME_ALLOCATOR->allocate_frame_by_count(KERNEL_FRAME_ALLOCATOR, 4, FRAME_ALLOCATION_TYPE_BLOCK | FRAME_ALLOCATION_TYPE_RESERVED, &queue_frames, NULL) != 0) {
            PRINTLOG(NVME, LOG_ERROR, "cannot allocate frame for queues");
            memory_free_ext(heap, nvme_disk);

            return -1;
        }

        uint64_t queue_va = MEMORY_PAGING_GET_VA_FOR_RESERVED_FA(queue_frames->frame_address);
        memory_paging_add_va_for_frame(queue_va, queue_frames, MEMORY_PAGING_PAGE_TYPE_NOEXEC);
        memory_memclean((void*)queue_va, FRAME_SIZE * 4);

        nvme_disk->admin_submission_queue = (nvme_submission_queue_entry_t*)queue_va;
        nvme_disk->admin_completion_queue = (nvme_completion_queue_entry_t*)(queue_va + FRAME_SIZE);
        nvme_disk->io_submission_queue = (nvme_submission_queue_entry_t*)(queue_va + FRAME_SIZE * 2);
        nvme_disk->io_completion_queue = (nvme_completion_queue_entry_t*)(queue_va + FRAME_SIZE * 3);

        nvme_disk->timeout = nvme_regs->capabilities.fields.timeout + 1;
        nvme_disk->nvme_registers = nvme_regs;

        nvme_regs->config.enable = 0;

        do {
            time_timer_spinsleep(500 * (nvme_regs->capabilities.fields.timeout + 1));
        } while(nvme_regs->status.ready);

        if(nvme_regs->status.cfs != 0) {
            PRINTLOG(NVME, LOG_ERROR, "error at disabling nvme: %i", nvme_regs->status.cfs);
            memory_free_ext(heap, nvme_disk);

            return -1;
        }

        PRINTLOG(NVME, LOG_DEBUG, "nvme asq %llx acq %llx", queue_frames->frame_address, queue_frames->frame_address + FRAME_SIZE);
        nvme_regs->config.css = 0;
        nvme_regs->asq = queue_frames->frame_address;
        nvme_regs->acq = queue_frames->frame_address + FRAME_SIZE;
        nvme_regs->aqa.acqs = 63;
        nvme_regs->aqa.asqs = 63;
        nvme_regs->config.iosqes = 6;
        nvme_regs->config.iocqes = 4;
        nvme_regs->config.ams = 0;
        nvme_regs->config.mps = 0;

        nvme_regs->config.enable = 1;

        do {
            time_timer_spinsleep(500 * (nvme_regs->capabilities.fields.timeout + 1));

            if(nvme_regs->status.cfs) {
                break;
            }
        } while(!nvme_regs->status.ready);

        if(nvme_regs->status.ready != 1) {
            PRINTLOG(NVME, LOG_ERROR, "cannot enable nvme: %i", nvme_regs->status.cfs);
            memory_free_ext(heap, nvme_disk);

            return -1;
        }

        uint64_t dstrd = nvme_regs->capabilities.fields.dstrd;
        uint64_t admin_sqtdb = bar_va + 0x1000 + (2 * 0) * (4 << dstrd);
        uint64_t admin_cqhdb = bar_va + 0x1000 + (2 * 0 + 1) * (4 << dstrd);
        uint64_t io_sqtdb = bar_va + 0x1000 + (2 * 1) * (4 << dstrd);
        uint64_t io_cqhdb = bar_va + 0x1000 + (2 * 1 + 1) * (4 << dstrd);

        PRINTLOG(NVME, LOG_DEBUG, "nvme admin sqtdb %llx cqhdb %llx", admin_sqtdb, admin_cqhdb);
        PRINTLOG(NVME, LOG_DEBUG, "nvme io sqtdb %llx cqhdb %llx", io_sqtdb, io_cqhdb);

        nvme_disk->admin_completion_queue_head_doorbell = (uint32_t*)admin_cqhdb;
        nvme_disk->admin_submission_queue_tail_doorbell = (uint32_t*)admin_sqtdb;
        nvme_disk->io_completion_queue_head_doorbell = (uint32_t*)io_cqhdb;
        nvme_disk->io_submission_queue_tail_doorbell = (uint32_t*)io_sqtdb;

        nvme_disk->next_cid = 1;

        frame_t* identify_frames = NULL;

        if(KERNEL_FRAME_ALLOCATOR->allocate_frame_by_count(KERNEL_FRAME_ALLOCATOR, 3, FRAME_ALLOCATION_TYPE_BLOCK | FRAME_ALLOCATION_TYPE_RESERVED, &identify_frames, NULL) != 0) {
            PRINTLOG(NVME, LOG_ERROR, "cannot allocate frame for identify");
            memory_free_ext(heap, nvme_disk);

            return -1;
        }

        uint64_t identify_va = MEMORY_PAGING_GET_VA_FOR_RESERVED_FA(identify_frames->frame_address);
        memory_paging_add_va_for_frame(identify_va, identify_frames, MEMORY_PAGING_PAGE_TYPE_NOEXEC);
        memory_memclean((void*)identify_va, 3 * FRAME_SIZE);

        nvme_disk->identify = (nvme_identify_t*)identify_va;
        nvme_disk->ns_identify = (nvme_ns_identify_t*)(identify_va + 0x1000);
        nvme_disk->active_ns_list = (uint32_t*)(identify_va + 0x2000);

        uint64_t identify_fa = identify_frames->frame_address;

        if(nvme_identify(nvme_disk, 1, 0, identify_fa) != 0) {
            PRINTLOG(NVME, LOG_ERROR, "cannot identify nvme controller");
            memory_free_ext(heap, nvme_disk);

            return -1;
        }

        PRINTLOG(NVME, LOG_DEBUG, "nvme controller has vwc? %x", nvme_disk->identify->vwc);
        nvme_disk->flush_supported = nvme_disk->identify->vwc & 1;

        if(nvme_disk->flush_supported && nvme_enable_cache(nvme_disk) == -1) {
            PRINTLOG(NVME, LOG_ERROR, "cannot enable nvme cache");
            memory_free_ext(heap, nvme_disk);

            return -1;
        }

        if(nvme_identify(nvme_disk, 2, 0, identify_fa + 0x2000) != 0) {
            PRINTLOG(NVME, LOG_ERROR, "cannot identify nvme active ns list");
            memory_free_ext(heap, nvme_disk);

            return -1;
        }

        for(int32_t i = 0; i < 0x1000 / 4; i++) {
            if(nvme_disk->active_ns_list[i] == 0) {
                break;
            }

            PRINTLOG(NVME, LOG_DEBUG, "ns: %x", nvme_disk->active_ns_list[i]);

            if(nvme_identify(nvme_disk, 0, nvme_disk->active_ns_list[i], identify_fa + 0x1000) != 0) {
                PRINTLOG(NVME, LOG_ERROR, "cannot identify nvme ns data");

                continue;
            }

            PRINTLOG(NVME, LOG_DEBUG, "ns capacity %llx", nvme_disk->ns_identify->ncap);
            PRINTLOG(NVME, LOG_DEBUG, "ns size %llx", nvme_disk->ns_identify->nsze);
            PRINTLOG(NVME, LOG_DEBUG, "ns lba format count %x", nvme_disk->ns_identify->nlbaf);
            PRINTLOG(NVME, LOG_DEBUG, "ns lba format %x", nvme_disk->ns_identify->flbas);
            PRINTLOG(NVME, LOG_DEBUG, "ns lba format size %x %x %x",
                     nvme_disk->ns_identify->lbaf[nvme_disk->ns_identify->flbas].ms,
                     nvme_disk->ns_identify->lbaf[nvme_disk->ns_identify->flbas].lbads,
                     nvme_disk->ns_identify->lbaf[nvme_disk->ns_identify->flbas].rp);

            nvme_disk->ns_id = nvme_disk->active_ns_list[i];
            nvme_disk->lba_count = nvme_disk->ns_identify->nsze;
            nvme_disk->lba_size = 1 << nvme_disk->ns_identify->lbaf[nvme_disk->ns_identify->flbas].lbads;
        }


        if(nvme_set_queue_count(nvme_disk, 1, 1) != 0) {
            PRINTLOG(NVME, LOG_ERROR, "cannot set queue count");
            memory_free_ext(heap, nvme_disk);

            return -1;
        }

        uint16_t cid = nvme_disk->next_cid++;

        PRINTLOG(NVME, LOG_DEBUG, "creating io cq");

        nvme_disk->io_queue_isr = pci_msix_set_isr(pci_nvme,  msix_cap, 1, nvme_isr, true);
        hashmap_put(nvme_disk_isr_map, (void*)nvme_disk->io_queue_isr, nvme_disk);

        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].opc = NVME_ADMIN_CMD_CREATE_CQ;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].fuse = 0;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cid = cid;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].nsid = 0;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].psdt = 0;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].mptr = 0;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].dptr.prplist.prp1 = queue_frames->frame_address + 3 * FRAME_SIZE;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].dptr.prplist.prp2 = 0;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw10 = 0x003f0001;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw11 = (1 << 16) | (1 << 1) | 1;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw12 = 0;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw13 = 0;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw14 = 0;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw15 = 0;


        nvme_disk->admin_s_queue_tail = (nvme_disk->admin_s_queue_tail + 1) % 64;
        *nvme_disk->admin_submission_queue_tail_doorbell = nvme_disk->admin_s_queue_tail;

        while(nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].cid != cid) {
            time_timer_spinsleep(500 * (nvme_regs->capabilities.fields.timeout + 1));
        }

        if(nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].status_code != 0 ||
           nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].status_type != 0) {
            PRINTLOG(NVME, LOG_ERROR, "cannot create io cq: %x",
                     nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].status_code);
            PRINTLOG(NVME, LOG_ERROR, "cannot create io cq: %x",
                     nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].status_type);
            memory_free_ext(heap, nvme_disk);

            return -1;
        }

        nvme_disk->admin_c_queue_head = (nvme_disk->admin_c_queue_head + 1) % 64;
        *nvme_disk->admin_completion_queue_head_doorbell = nvme_disk->admin_c_queue_head;

        pci_msix_clear_pending_bit(pci_nvme, msix_cap, 1, true);

        PRINTLOG(NVME, LOG_DEBUG, "io cq created");

        PRINTLOG(NVME, LOG_DEBUG, "creating io sq");

        cid = nvme_disk->next_cid++;

        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].opc = NVME_ADMIN_CMD_CREATE_SQ;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].fuse = 0;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cid = cid;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].nsid = 0;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].psdt = 0;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].mptr = 0;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].dptr.prplist.prp1 = queue_frames->frame_address + 2 * FRAME_SIZE;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].dptr.prplist.prp2 = 0;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw10 = 0x003f0001;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw11 = (1 << 16) | 1;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw12 = 0;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw13 = 0;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw14 = 0;
        nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw15 = 0;


        nvme_disk->admin_s_queue_tail = (nvme_disk->admin_s_queue_tail + 1) % 64;
        *nvme_disk->admin_submission_queue_tail_doorbell = nvme_disk->admin_s_queue_tail;

        while(nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].cid != cid) {
            time_timer_spinsleep(500 * (nvme_regs->capabilities.fields.timeout + 1));
        }

        if(nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].status_code != 0 ||
           nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].status_type != 0) {
            PRINTLOG(NVME, LOG_ERROR, "cannot create io sq: %x",
                     nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].status_code);
            PRINTLOG(NVME, LOG_ERROR, "cannot create io sq: %x",
                     nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].status_type);
            memory_free_ext(heap, nvme_disk);

            return -1;
        }

        nvme_disk->admin_c_queue_head = (nvme_disk->admin_c_queue_head + 1) % 64;
        *nvme_disk->admin_completion_queue_head_doorbell = nvme_disk->admin_c_queue_head;

        PRINTLOG(NVME, LOG_DEBUG, "io sq created");

        frame_t* prp_frames = NULL;

        if(KERNEL_FRAME_ALLOCATOR->allocate_frame_by_count(KERNEL_FRAME_ALLOCATOR, 64, FRAME_ALLOCATION_TYPE_BLOCK | FRAME_ALLOCATION_TYPE_RESERVED, &prp_frames, NULL) != 0) {
            PRINTLOG(NVME, LOG_ERROR, "cannot allocate frame for prp");
            memory_free_ext(heap, nvme_disk);

            return -1;
        }

        nvme_disk->prp_frame_fa = prp_frames->frame_address;
        nvme_disk->prp_frame_va = MEMORY_PAGING_GET_VA_FOR_RESERVED_FA(nvme_disk->prp_frame_fa);

        memory_paging_add_va_for_frame(nvme_disk->prp_frame_va, prp_frames, MEMORY_PAGING_PAGE_TYPE_NOEXEC);

        hashmap_put(nvme_disks, (void*)nvme_disk->disk_id, nvme_disk);

        disk_id++;
        iter = iter->next(iter);
    }

    iter->destroy(iter);

    return hashmap_size(nvme_disks);
}
#pragma GCC diagnostic pop

int8_t nvme_enable_cache(nvme_disk_t* nvme_disk) {
    PRINTLOG(NVME, LOG_DEBUG, "enabling cache");

    uint16_t cid = nvme_disk->next_cid++;

    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].opc = NVME_ADMIN_CMD_SET_FEATURES;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].fuse = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cid = cid;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].nsid = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].psdt = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].mptr = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].dptr.prplist.prp1 = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].dptr.prplist.prp2 = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw10 = 0x6;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw11 = 1;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw12 = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw13 = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw14 = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw15 = 0;


    nvme_disk->admin_s_queue_tail = (nvme_disk->admin_s_queue_tail + 1) % 64;
    *nvme_disk->admin_submission_queue_tail_doorbell = nvme_disk->admin_s_queue_tail;

    while(nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].cid != cid) {
        time_timer_spinsleep(500 * nvme_disk->timeout);
    }

    int8_t res = 0;

    if(nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].status_code != 0 ||
       nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].status_type != 0) {
        PRINTLOG(NVME, LOG_ERROR, "cannot enable cache: %x",
                 nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].status_code);
        PRINTLOG(NVME, LOG_ERROR, "cannot cache: %x",
                 nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].status_type);

        res = -1;
    }

    nvme_disk->admin_c_queue_head = (nvme_disk->admin_c_queue_head + 1) % 64;
    *nvme_disk->admin_completion_queue_head_doorbell = nvme_disk->admin_c_queue_head;

    PRINTLOG(NVME, LOG_DEBUG, "cache enabled");

    return res;
}

int8_t nvme_set_queue_count(nvme_disk_t* nvme_disk, uint16_t io_sq_count, uint16_t io_cq_count) {
    PRINTLOG(NVME, LOG_DEBUG, "enabling io queues with %d sq and %d cq", io_sq_count, io_cq_count);

    uint16_t cid = nvme_disk->next_cid++;
    io_sq_count--;
    io_cq_count--;

    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].opc = NVME_ADMIN_CMD_SET_FEATURES;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].fuse = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cid = cid;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].nsid = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].psdt = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].mptr = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].dptr.prplist.prp1 = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].dptr.prplist.prp2 = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw10 = 0x7;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw11 = (io_cq_count << 16) | io_sq_count;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw12 = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw13 = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw14 = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw15 = 0;


    nvme_disk->admin_s_queue_tail = (nvme_disk->admin_s_queue_tail + 1) % 64;
    *nvme_disk->admin_submission_queue_tail_doorbell = nvme_disk->admin_s_queue_tail;

    while(nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].cid != cid) {
        time_timer_spinsleep(500 * nvme_disk->timeout);
    }

    int8_t res = 0;

    if(nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].status_code != 0 ||
       nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].status_type != 0) {
        PRINTLOG(NVME, LOG_ERROR, "cannot set queue counts: %x",
                 nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].status_code);
        PRINTLOG(NVME, LOG_ERROR, "cannot set queue counts: %x",
                 nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].status_type);

        res = -1;
    }

    nvme_disk->io_sq_count = (nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].cdw0 & 0xFFFF) + 1;
    nvme_disk->io_cq_count = ((nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].cdw0 >> 16) & 0xFFFF) + 1;

    PRINTLOG(NVME, LOG_DEBUG, "queue counts set with %d sq and %d cq",
             nvme_disk->io_sq_count, nvme_disk->io_cq_count);


    nvme_disk->admin_c_queue_head = (nvme_disk->admin_c_queue_head + 1) % 64;
    *nvme_disk->admin_completion_queue_head_doorbell = nvme_disk->admin_c_queue_head;

    PRINTLOG(NVME, LOG_DEBUG, "io queues enabled");

    return res;
}

int8_t nvme_identify(nvme_disk_t* nvme_disk,  uint32_t cns, uint32_t nsid, uint64_t data_address) {
    PRINTLOG(NVME, LOG_DEBUG, "querying identify");

    uint16_t cid = nvme_disk->next_cid++;

    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].opc = NVME_ADMIN_CMD_IDENTIFY;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].fuse = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cid = cid;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].nsid = nsid;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].psdt = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].mptr = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].dptr.prplist.prp1 = data_address;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].dptr.prplist.prp2 = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw10 = cns;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw11 = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw12 = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw13 = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw14 = 0;
    nvme_disk->admin_submission_queue[nvme_disk->admin_s_queue_tail].cdw15 = 0;


    nvme_disk->admin_s_queue_tail = (nvme_disk->admin_s_queue_tail + 1) % 64;
    *nvme_disk->admin_submission_queue_tail_doorbell = nvme_disk->admin_s_queue_tail;

    while(nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].cid != cid) {
        time_timer_spinsleep(500 * nvme_disk->timeout);
    }

    int8_t res = 0;

    if(nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].status_code != 0 ||
       nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].status_type != 0) {
        PRINTLOG(NVME, LOG_ERROR, "cannot identify: %x",
                 nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].status_code);
        PRINTLOG(NVME, LOG_ERROR, "cannot identify: %x",
                 nvme_disk->admin_completion_queue[nvme_disk->admin_c_queue_head].status_type);

        res = -1;
    }

    nvme_disk->admin_c_queue_head = (nvme_disk->admin_c_queue_head + 1) % 64;
    *nvme_disk->admin_completion_queue_head_doorbell = nvme_disk->admin_c_queue_head;

    PRINTLOG(NVME, LOG_DEBUG, "identify completed");

    return res;
}

future_t nvme_read(uint64_t disk_id, uint64_t lba, uint16_t size, uint8_t* buffer) {
    return nvme_read_write(disk_id, lba, size, buffer, false);
}

future_t nvme_write(uint64_t disk_id, uint64_t lba, uint16_t size, uint8_t* buffer) {
    return nvme_read_write(disk_id, lba, size, buffer, true);
}

future_t nvme_read_write(uint64_t disk_id, uint64_t lba, uint16_t size, uint8_t* buffer, boolean_t write) {
    if(size % 0x1000) {
        PRINTLOG(NVME, LOG_ERROR, "cannot %s: size not multiple of 4k", write?"write":"read");

        return NULL;
    }

    uint64_t fcnt = size / 0x1000;

    if(fcnt > 512) {
        PRINTLOG(NVME, LOG_ERROR, "cannot %s: size too big", write?"write":"read");

        return NULL;
    }

    uint64_t buffer_va = (uint64_t)buffer;

    if(buffer_va % 0x1000) {
        PRINTLOG(NVME, LOG_ERROR, "cannot %s: buffer not aligned to 4k", write?"write":"read");

        return NULL;
    }

    uint64_t buffer_fa = 0;

    if(memory_paging_get_physical_address(buffer_va, &buffer_fa) != 0) {
        PRINTLOG(NVME, LOG_ERROR, "cannot %s: buffer physical address not found", write?"write":"read");

        return NULL;
    }

    nvme_disk_t* nvme_disk = (nvme_disk_t*)hashmap_get(nvme_disks, (void*)disk_id);

    if(nvme_disk == NULL) {
        PRINTLOG(NVME, LOG_ERROR, "cannot %s: disk not found", write?"write":"read");

        return NULL;
    }

    uint16_t cid = nvme_disk->next_cid++;

    uint64_t iosqt = nvme_disk->io_s_queue_tail;

    uint64_t prp1 = buffer_fa;

    uint64_t prp2 = 0;

    if(fcnt == 2) {
        prp2 = prp1 + 0x1000;
    } else if(fcnt > 2) {
        prp2 = nvme_disk->prp_frame_fa + iosqt * 0x1000;
        uint64_t* prp2_list = (uint64_t*)nvme_disk->prp_frame_va + iosqt * 0x1000;
        memory_memclean(prp2_list, 0x1000);

        for(uint64_t i = 0; i < fcnt - 1; i++) {
            prp2_list[i] = prp1 + (i + 1) * 0x1000;
        }
    }

    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].opc = write?NVME_CMD_WRITE:NVME_CMD_READ;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].fuse = 0;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].cid = cid;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].nsid = nvme_disk->ns_id;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].psdt = 0;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].mptr = 0;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].dptr.prplist.prp1 = prp1;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].dptr.prplist.prp2 = prp2;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].cdw10 = lba & 0xFFFFFFFF;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].cdw11 = (lba >> 32) & 0xFFFFFFFF;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].cdw12 = fcnt - 1;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].cdw13 = 0;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].cdw14 = 0;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].cdw15 = 0;

    lock_t lock = lock_create_with_heap_for_future(nvme_disk->heap, true);
    hashmap_put(nvme_disk->command_lock_map, (void*)(uint64_t)cid, lock);
    future_t fut = future_create(lock);

    nvme_disk->io_s_queue_tail = (nvme_disk->io_s_queue_tail + 1) % 64;
    *nvme_disk->io_submission_queue_tail_doorbell = nvme_disk->io_s_queue_tail;

    PRINTLOG(NVME, LOG_DEBUG, "%s command sent with cid %x and s tail %llx", write?"write":"read", cid, nvme_disk->io_s_queue_tail - 1);

    return fut;
}

future_t nvme_flush(uint64_t disk_id) {
    PRINTLOG(NVME, LOG_DEBUG, "flushing disk %llx", disk_id);

    nvme_disk_t* nvme_disk = (nvme_disk_t*)hashmap_get(nvme_disks, (void*)disk_id);

    if(nvme_disk == NULL) {
        PRINTLOG(NVME, LOG_ERROR, "cannot flush: disk not found");

        return NULL;
    }

    if(!nvme_disk->flush_supported) {
        PRINTLOG(NVME, LOG_DEBUG, "cannot flush: flush not supported");

        return NULL;
    }

    uint16_t cid = nvme_disk->next_cid++;

    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].opc = NVME_CMD_FLUSH;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].fuse = 0;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].cid = cid;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].nsid = 0xFFFFFFFF;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].psdt = 0;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].mptr = 0;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].dptr.prplist.prp1 = 0;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].dptr.prplist.prp2 = 0;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].cdw10 = 0;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].cdw11 = 0;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].cdw12 = 0;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].cdw13 = 0;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].cdw14 = 0;
    nvme_disk->io_submission_queue[nvme_disk->io_s_queue_tail].cdw15 = 0;

    lock_t lock = lock_create_with_heap_for_future(nvme_disk->heap, true);
    hashmap_put(nvme_disk->command_lock_map, (void*)(uint64_t)cid, lock);
    future_t fut = future_create(lock);

    nvme_disk->io_s_queue_tail = (nvme_disk->io_s_queue_tail + 1) % 64;
    *nvme_disk->io_submission_queue_tail_doorbell = nvme_disk->io_s_queue_tail;

    PRINTLOG(NVME, LOG_DEBUG, "flush command sent with cid %x and s tail %llx", cid, nvme_disk->io_s_queue_tail - 1);

    return fut;
}
