#include <vgic.h>
#include <hvmm_trace.h>
#include <armv7_p15.h>
#include <gic.h>
#include <gic_regs.h>
#include <slotpirq.h>
#include <context.h>
#include "print.h"
#include "hyp_config.h"
#include <asm-arm_inline.h>

/* for test, surpress traces */
//#define __VGIC_DISABLE_TRACE__
#define VGIC_SIMULATE_HWVIRQ

#ifdef __VGIC_DISABLE_TRACE__
#ifdef HVMM_TRACE_ENTER
#undef HVMM_TRACE_ENTER
#undef HVMM_TRACE_EXIT
#undef HVMM_TRACE_HEX32
#define HVMM_TRACE_ENTER()
#define HVMM_TRACE_EXIT()
#define HVMM_TRACE_HEX32(a,b)
#endif
#endif

/* Cortex-A15: 25 (PPI6) */
#define VGIC_MAINTENANCE_INTERRUPT_IRQ  25

#define VGIC_MAX_LISTREGISTERS          VGIC_NUM_MAX_SLOTS
#define VGIC_SIGNATURE_INITIALIZED      0x45108EAD
#define VGIC_READY()                    (_vgic.initialized == VGIC_SIGNATURE_INITIALIZED) 

/*
 * Operations:
 * - INIT - [V] Number of List Registers
 * - INIT - [V] route/enable maintenance IRQ
 * - INIT - [V] enable VGIC
 * - INIT - [V] Enable/Disable Virtual IRQ HCR.VI[7]
 *       - vgic_inject_enable()
 * - [V] Inject virq, slot(lr), hw?, state=pending,priority,
 *      - hw:1 - physicalID
 *      - hw:0 - cpuid, EOI(->maintenance int)
 *      GICH_ELSR[VIRQ/32][VIRQ%32] == 1, Free
 *      Otherwise, Used
 *
 *      - vgic_inject_virq( virq, slot, state, priority, hw, physrc, maintenance )
 *      - vgic_inject_virq_hw( virq, state, priority, pirq)
 *      - vgic_inject_virq_sw( virq, state, priority, cpuid, maintenance )
 *
 *  - [*] ISR: Maintenance IRQ
 *      Check VICH_MISR
 *          [V] EOI - At least one VIRQ EOI
 *          [ ] U - Underflow - Non or one valid interrupt in LRs
 *          [ ] LRENP - LI Entry Not Present (no valid interrupt for an EOI request)
 *          [ ] NP - No Pending Interrupt
 *          [ ] VGrp[0/1][E/D] 
 *  - [V] Context Switch:
 *  Saved/Restored Registers:
 *      - GICH_LR
 *      - GICH_APR
 *      - GICH_HCR
 *      - GICH_VMCR
 *  Saved/Restored Data:
 *      - Free Interrupts
 *
 */

struct vgic {
    volatile uint32_t *base;    /* Base address of VGIC (Virtual Interface Control Registers) */
    uint32_t num_lr;            /* Number of List Registers */
    uint32_t initialized;       /* vgic module initialized if == VGIC_SIGNATURE_INITIALIZED */
    uint64_t valid_lr_mask;
};

hvmm_status_t vgic_injection_enable(uint8_t enable);

static struct vgic _vgic;
static void (*_cb_virq_flush)(vmid_t vmid) = 0;

static uint32_t vgic_find_free_slot(void)
{
    uint32_t slot;
    uint32_t shift = 0;

    slot = _vgic.base[GICH_ELSR0];
    if ( slot == 0 && _vgic.num_lr > 32 ) {
        /* first 32 slots are occupied, try the later */
        slot = _vgic.base[GICH_ELSR1];
        shift = 32;
    }


    if ( slot ) {
        slot &= -(slot);
        slot = (31 - asm_clz(slot));
        slot += shift;
    } else {
        /* 64 slots are fully occupied */
        slot = VGIC_SLOT_NOTFOUND;
    }
    return slot;
}

/*
 * Test if the List Registers 'slot' is free
 * Return
 *  Free - 'slot' is returned
 *  Occupied but another free slot found - new free slot
 *  Fully occupied - VGIC_SLOT_NOTFOUND
 */

static uint32_t vgic_is_free_slot(uint32_t slot)
{
    uint32_t free_slot = VGIC_SLOT_NOTFOUND;
    
    if ( slot < 32 ) {
        if ( _vgic.base[GICH_ELSR0] & (1 << slot) )
            free_slot = slot;
    } else {
        if ( _vgic.base[GICH_ELSR1] & (1 << (slot - 32)) )
            free_slot = slot;
    }

    if ( free_slot != slot ) {
        free_slot = vgic_find_free_slot();
    }

    return free_slot;
}

static void _vgic_dump_status(void)
{
    /*
     * === VGIC Status Summary ===
     * Initialized: Yes
     * Num ListRegs: n
     * Hypervisor Control
     *  - Enabled: Yes
     *  - EOICount: 
     *  - Underflow:
     *  - LRENPIE:
     *  - NPIE:
     *  - VGrp0EIE:
     *  - VGrp0DIE:
     *  - VGrp1EIE:
     *  - VGrp1DIE:
     * VGIC Type
     *  - ListRegs:
     *  - PREbits:
     *  - PRIbits:
     * Virtual Machine Control
     *  - 
     */
    uart_print("=== VGIC Status ===\n\r");
    uart_print(" Initialized:"); uart_print( ( VGIC_READY() ? "Yes" : "No" ) ); uart_print("\n\r");
    uart_print(" Num ListRegs:"); uart_print_hex32( _vgic.num_lr ); uart_print("\n\r");
    uart_print(" LR_MASK:"); uart_print_hex64( _vgic.valid_lr_mask ); uart_print("\n\r");
}

static void _vgic_dump_regs(void)
{
#ifndef __VGIC_DISABLE_TRACE__
    /*
     * HCR * VTR * VMCR * MISR * EISR0 * EISR1 * ELSR0 * ELSR1 * APR * LR0~n
     */
    int i;
    HVMM_TRACE_ENTER();

    uart_print("  hcr:"); uart_print_hex32( _vgic.base[GICH_HCR] ); uart_print("\n\r");
    uart_print("  vtr:"); uart_print_hex32( _vgic.base[GICH_VTR] ); uart_print("\n\r");
    uart_print(" vmcr:"); uart_print_hex32( _vgic.base[GICH_VMCR] ); uart_print("\n\r");
    uart_print(" misr:"); uart_print_hex32( _vgic.base[GICH_MISR] ); uart_print("\n\r");
    uart_print("eisr0:"); uart_print_hex32( _vgic.base[GICH_EISR0] ); uart_print("\n\r");
    uart_print("eisr1:"); uart_print_hex32( _vgic.base[GICH_EISR1] ); uart_print("\n\r");
    uart_print("elsr0:"); uart_print_hex32( _vgic.base[GICH_ELSR0] ); uart_print("\n\r");
    uart_print("elsr1:"); uart_print_hex32( _vgic.base[GICH_ELSR1] ); uart_print("\n\r");
    uart_print("  apr:"); uart_print_hex32( _vgic.base[GICH_APR] ); uart_print("\n\r");

    uart_print("   LR:\n\r"); 
    for( i = 0; i < _vgic.num_lr; i++ ) {
        if ( vgic_is_free_slot(i) != i ) {
            uart_print_hex32( _vgic.base[GICH_LR + i] ); uart_print(" - "); uart_print_hex32(i); uart_print("\n\r");
        }
    }

    HVMM_TRACE_EXIT();
#endif
}

static void _vgic_isr_maintenance_irq(int irq, void *pregs, void *pdata)
{
    HVMM_TRACE_ENTER();

    if ( _vgic.base[GICH_MISR] & GICH_MISR_EOI ) {
        /* clean up invalid entries from List Registers */
        uint32_t eisr = _vgic.base[GICH_EISR0];
        uint32_t slot;
        uint32_t pirq;
        vmid_t vmid;

        vmid = context_current_vmid();
        while(eisr) {
            slot = (31 - asm_clz(eisr));
            eisr &= ~(1 << slot);
            _vgic.base[GICH_LR + slot] = 0;

            /* deactivate associated pirq at the slot */
            pirq = slotpirq_get(vmid, slot);
            if ( pirq != PIRQ_INVALID ) {
                gic_deactivate_irq(pirq);
                slotpirq_clear(vmid, slot);
                printh( "vgic: deactivated pirq %d at slot %d\n", pirq, slot );
            } else {
                printh( "vgic: deactivated virq at slot %d\n", slot );
            }
        }

        eisr = _vgic.base[GICH_EISR1];
        while(eisr) {
            slot = (31 - asm_clz(eisr));
            eisr &= ~(1 << slot);
            _vgic.base[GICH_LR + slot + 32] = 0;

            /* deactivate associated pirq at the slot */
            pirq = slotpirq_get(vmid, slot + 32);
            if ( pirq != PIRQ_INVALID ) {
                gic_deactivate_irq(pirq);
                slotpirq_clear(vmid, slot + 32);
                printh( "vgic: deactivated pirq %d at slot %d\n", pirq, slot );
            } else {
                printh( "vgic: deactivated virq at slot %d\n", slot );
            }
        }
    }

    if ( _vgic.base[GICH_MISR] & GICH_MISR_NP ) {
        /* No pending virqs, no need to keep vgic enabled */
        _vgic.base[GICH_HCR] &= ~(GICH_HCR_NPIE);
        printh( "vgic: no pending virqs, disabling no pending interrupt\n" );
        {
            int i;
            printh( "vgic: active virqs...\n" );
            for (i = 0; i < _vgic.num_lr; i++ ) {
                if ( _vgic.base[GICH_LR + i] & 0x20000000 ) {
                    printh( "- lr[%d]: %x\n", i, _vgic.base[GICH_LR + i] );
                }
            }
        }
    }

    if ( ((~(_vgic.base[GICH_ELSR0])) | (~(_vgic.base[GICH_ELSR1]))) == 0 ) {
        /* No valid interrupt */
        vgic_enable(0);
        vgic_injection_enable(0);
        printh( "vgic: no valid virqs, disabling vgic\n" );
    } else {
        printh( "vgic:MISR:%x ELSR0:%x ELSR1:%x\n",
            _vgic.base[GICH_MISR], 
            _vgic.base[GICH_ELSR0],
            _vgic.base[GICH_ELSR1]);
    }

    HVMM_TRACE_EXIT();
}

hvmm_status_t vgic_enable(uint8_t enable)
{
    hvmm_status_t result = HVMM_STATUS_BAD_ACCESS;

    if ( VGIC_READY() ) {

        if ( enable ) {
            uint32_t hcr = _vgic.base[GICH_HCR];

            hcr |= GICH_HCR_EN | GICH_HCR_NPIE;

            _vgic.base[GICH_HCR] = hcr;
        } else {
            _vgic.base[GICH_HCR] &= ~(GICH_HCR_EN | GICH_HCR_NPIE);
        }

        result = HVMM_STATUS_SUCCESS;
    } 
    return result;
}

hvmm_status_t vgic_injection_enable(uint8_t enable)
{
    uint32_t hcr;

    hcr = read_hcr();
    if ( enable ) {
        if ( (hcr & HCR_VI) == 0 ) {
            hcr |= HCR_VI;
            write_hcr(hcr);
        }
    } else {
        if ( hcr & HCR_VI ) {
            hcr &= ~(HCR_VI);
            write_hcr(hcr);
        }
    }

	hcr = read_hcr(); uart_print( " updated hcr:"); uart_print_hex32(hcr); uart_print("\n\r");
    return HVMM_STATUS_SUCCESS;
}

/*
 * Params
 * @virq            virtual id (seen to the guest as an IRQ)
 * @slot            index to GICH_LR, slot < _vgic.num_lr
 * @state           INACTIVE, PENDING, ACTIVE, or PENDING_ACTIVE
 * @priority        5bit priority
 * @hw              1 - physical interrupt, 0 - otherwise
 * @physrc          hw:1 - Physical ID, hw:0 - CPUID
 * @maintenance     hw:0, requires EOI asserts Virtual Maintenance Interrupt
 *
 * @return          slot index, or VGIC_SLOT_NOTFOUND
 */
uint32_t vgic_inject_virq( 
        uint32_t virq, uint32_t slot, virq_state_t state, uint32_t priority, 
        uint8_t hw, uint32_t physrc, uint8_t maintenance )
{
    uint32_t physicalid;
    uint32_t lr_desc;

    HVMM_TRACE_ENTER();

    physicalid = (hw ? physrc : (maintenance << 9) | (physrc & 0x7)) << GICH_LR_PHYSICALID_SHIFT;
    physicalid &= GICH_LR_PHYSICALID_MASK;


    lr_desc = (GICH_LR_HW_MASK & (hw << GICH_LR_HW_SHIFT) ) |
        /* (GICH_LR_GRP1_MASK & (1 << GICH_LR_GRP1_SHIFT) )| */
        (GICH_LR_STATE_MASK & (state << GICH_LR_STATE_SHIFT) ) |
        (GICH_LR_PRIORITY_MASK & ( (priority >> 3)  << GICH_LR_PRIORITY_SHIFT) ) |
        physicalid |
        (GICH_LR_VIRTUALID_MASK & virq );

    slot = vgic_is_free_slot( slot );

    HVMM_TRACE_HEX32("lr_desc:", lr_desc);
    HVMM_TRACE_HEX32("free slot:", slot);

    if ( slot != VGIC_SLOT_NOTFOUND ) {
        _vgic.base[GICH_LR + slot] = lr_desc;
        vgic_injection_enable(1);
        vgic_enable(1);
    }
    _vgic_dump_regs();

    HVMM_TRACE_EXIT();
    return slot;
}

/*
 * Return: slot index if successful, VGIC_SLOT_NOTFOUND otherwise 
 */
uint32_t vgic_inject_virq_hw( uint32_t virq, virq_state_t state, uint32_t priority, uint32_t pirq)
{
    uint32_t slot = VGIC_SLOT_NOTFOUND;
    HVMM_TRACE_ENTER();

    slot = vgic_find_free_slot();
    HVMM_TRACE_HEX32("slot:", slot);
    if ( slot != VGIC_SLOT_NOTFOUND ) {
#ifdef VGIC_SIMULATE_HWVIRQ
        slot = vgic_inject_virq( virq, slot, state, priority, 0, 0, 1 );
#else
        slot = vgic_inject_virq( virq, slot, state, priority, 1, pirq, 0 );
#endif
    }

    HVMM_TRACE_EXIT();
    return slot;
}

uint32_t vgic_inject_virq_sw( uint32_t virq, virq_state_t state, uint32_t priority, uint32_t cpuid, uint8_t maintenance)
{
    uint32_t slot = VGIC_SLOT_NOTFOUND;
    HVMM_TRACE_ENTER();

    slot = vgic_find_free_slot();
    HVMM_TRACE_HEX32("slot:", slot);
    if ( slot != VGIC_SLOT_NOTFOUND ) {
        slot = vgic_inject_virq( virq, slot, state, priority, 0, cpuid, maintenance );
    }

    HVMM_TRACE_EXIT();
    return slot;
}


hvmm_status_t vgic_maintenance_irq_enable(uint8_t enable)
{
    uint32_t irq = VGIC_MAINTENANCE_INTERRUPT_IRQ;

    HVMM_TRACE_ENTER();
    if ( enable ) {
        gic_test_set_irq_handler( irq, &_vgic_isr_maintenance_irq, 0 );
        gic_test_configure_irq( irq,
                GIC_INT_POLARITY_LEVEL,
                gic_cpumask_current(),
                GIC_INT_PRIORITY_DEFAULT );
    } else {
        gic_test_set_irq_handler( irq, 0, 0 );
        gic_disable_irq( irq );
    }
    HVMM_TRACE_EXIT();
    return HVMM_STATUS_SUCCESS;
}

static uint64_t _vgic_valid_lr_mask( uint32_t num_lr )
{
    uint64_t mask_valid_lr = 0xFFFFFFFFFFFFFFFFULL;
    if ( num_lr < VGIC_MAX_LISTREGISTERS ) {
        mask_valid_lr >>= num_lr;
        mask_valid_lr <<= num_lr;
        mask_valid_lr = ~mask_valid_lr;
    }

    return mask_valid_lr;
}

/* 
 * Registers the callback for flushing out queued virqs for the specified guest (vmid)
 */
hvmm_status_t vgic_setcallback_virq_flush(void (*callback)(vmid_t vmid))
{
    _cb_virq_flush = callback;
    if ( _cb_virq_flush == 0 ) {
        printh( "vgic: virq_flush() cleared\n" );
    } else {
        printh( "vgic: virq_flush() set to function at %x\n", (uint32_t) _cb_virq_flush );
    }
    return HVMM_STATUS_SUCCESS;
}

hvmm_status_t vgic_init(void)
{
    hvmm_status_t result = HVMM_STATUS_UNKNOWN_ERROR;

    HVMM_TRACE_ENTER();

    _vgic.base = gic_vgic_baseaddr();
    _vgic.num_lr = (_vgic.base[GICH_VTR] & GICH_VTR_LISTREGS_MASK) + 1;
    _vgic.valid_lr_mask = _vgic_valid_lr_mask( _vgic.num_lr );
    _vgic.initialized = VGIC_SIGNATURE_INITIALIZED;

    vgic_maintenance_irq_enable(1);

    slotpirq_init();

    result = HVMM_STATUS_SUCCESS;

    _vgic_dump_status();
    _vgic_dump_regs();

    HVMM_TRACE_EXIT();
    return result;
}

hvmm_status_t vgic_init_status( struct vgic_status *status, vmid_t vmid)
{
    hvmm_status_t result = HVMM_STATUS_SUCCESS;
    int i;

    status->hcr = 0;
    status->apr = 0;
    status->vmcr = 0;
    status->saved_once = 0;
    for( i = 0; i < _vgic.num_lr; i++) {
        status->lr[i] = 0;
    }

    return result;
}

hvmm_status_t vgic_save_status( struct vgic_status *status, vmid_t vmid )
{
    hvmm_status_t result = HVMM_STATUS_SUCCESS;
    int i;


    for( i = 0; i < _vgic.num_lr; i++ ) {
        status->lr[i] = _vgic.base[GICH_LR + i];
    }
    status->hcr = _vgic.base[GICH_HCR];
    status->apr = _vgic.base[GICH_APR];
    status->vmcr = _vgic.base[GICH_VMCR];
    status->saved_once = VGIC_SIGNATURE_INITIALIZED;

    vgic_enable(0);
    return result;
}

hvmm_status_t vgic_restore_status( struct vgic_status *status, vmid_t vmid )
{
    hvmm_status_t result = HVMM_STATUS_BAD_ACCESS;
    int i;

    for( i = 0; i < _vgic.num_lr; i++) {
        _vgic.base[GICH_LR + i] = status->lr[i];
    }
    _vgic.base[GICH_APR] = status->apr;
    _vgic.base[GICH_VMCR] = status->vmcr;
    _vgic.base[GICH_HCR] = status->hcr;

    /* Inject queued virqs to the next guest */
    vgic_flush_virqs(vmid);

    _vgic_dump_regs();
    result = HVMM_STATUS_SUCCESS;

    return result;
}

hvmm_status_t vgic_flush_virqs(vmid_t vmid)
{
    hvmm_status_t result = HVMM_STATUS_IGNORED;
    if ( _cb_virq_flush != 0 ) {
        _cb_virq_flush(vmid);
        result = HVMM_STATUS_SUCCESS;
    }

    return result;
}

