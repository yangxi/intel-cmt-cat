/*
 * BSD LICENSE
 *
 * Copyright(c) 2014-2016 Intel Corporation. All rights reserved.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.O
 *
 */

/**
 * @brief Implementation of CAT releated PQoS API
 *
 * CPUID and MSR operations are done on the 'local'/host system.
 * Module operate directly on CAT registers.
 */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "pqos.h"

#include "host_cap.h"
#include "host_allocation.h"

#include "machine.h"
#include "types.h"
#include "log.h"

/**
 * ---------------------------------------
 * Local macros
 * ---------------------------------------
 */

/**
 * Allocation & Monitoring association MSR register
 *
 * [63..<QE COS>..32][31..<RESERVED>..10][9..<RMID>..0]
 */
#define PQOS_MSR_ASSOC             0xC8F
#define PQOS_MSR_ASSOC_QECOS_SHIFT 32
#define PQOS_MSR_ASSOC_QECOS_MASK  0xffffffff00000000ULL

/**
 * Allocation class of service (COS) MSR registers
 */
#define PQOS_MSR_L3CA_MASK_START 0xC90
#define PQOS_MSR_L3CA_MASK_END   0xD0F
#define PQOS_MSR_L3CA_MASK_NUMOF \
        (PQOS_MSR_L3CA_MASK_END - PQOS_MSR_L3CA_MASK_START + 1)

#define PQOS_MSR_L2CA_MASK_START 0xD10
#define PQOS_MSR_L2CA_MASK_END   0xD4F
#define PQOS_MSR_L2CA_MASK_NUMOF \
        (PQOS_MSR_L2CA_MASK_END - PQOS_MSR_L2CA_MASK_START + 1)

#define PQOS_MSR_L3_QOS_CFG          0xC81   /**< CAT config register */
#define PQOS_MSR_L3_QOS_CFG_CDP_EN   1ULL    /**< CDP enable bit */

/**
 * ---------------------------------------
 * Local data types
 * ---------------------------------------
 */

/**
 * ---------------------------------------
 * Local data structures
 * ---------------------------------------
 */
const struct pqos_cap *m_cap = NULL;
const struct pqos_cpuinfo *m_cpu = NULL;

/**
 * ---------------------------------------
 * External data
 * ---------------------------------------
 */

/**
 * =======================================
 * =======================================
 *
 * initialize and shutdown
 *
 * =======================================
 * =======================================
 */

int
pqos_alloc_init(const struct pqos_cpuinfo *cpu,
                const struct pqos_cap *cap,
                const struct pqos_config *cfg)
{
        int ret = PQOS_RETVAL_OK;

        UNUSED_PARAM(cfg);
        m_cap = cap;
        m_cpu = cpu;
        return ret;
}

int
pqos_alloc_fini(void)
{
        int ret = PQOS_RETVAL_OK;

        m_cap = NULL;
        m_cpu = NULL;
        return ret;
}

/**
 * =======================================
 * L3 cache allocation
 * =======================================
 */

/**
 * @brief Tests if \a bitmask is contiguous
 *
 * Zero bit mask is regarded as not contiguous.
 *
 * The function shifts out first group of contiguous 1's in the bit mask.
 * Next it checks remaining bitmask content to make a decision.
 *
 * @param bitmask bit mask to be validated for contiguity
 *
 * @return Bit mask contiguity check result
 * @retval 0 not contiguous
 * @retval 1 contiguous
 */
static int is_contiguous(uint64_t bitmask)
{
        if (bitmask == 0)
                return 0;

        while ((bitmask & 1) == 0) /**< Shift until 1 found at position 0 */
                bitmask >>= 1;

        while ((bitmask & 1) != 0) /**< Shift until 0 found at position 0 */
                bitmask >>= 1;

        return (bitmask) ? 0 : 1;  /**< non-zero bitmask is not contiguous */
}

int
pqos_l3ca_set(const unsigned socket,
              const unsigned num_ca,
              const struct pqos_l3ca *ca)
{
        int ret = PQOS_RETVAL_OK;
        unsigned i = 0, count = 0, core = 0;
        int cdp_enabled = 0;

        _pqos_api_lock();

        ret = _pqos_check_init(1);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return ret;
        }

        if (ca == NULL || num_ca == 0) {
                _pqos_api_unlock();
                return PQOS_RETVAL_PARAM;
        }

        /**
         * Check if class bitmasks are contiguous.
         */
        for (i = 0; i < num_ca; i++) {
                int is_contig = 0;

                if (ca[i].cdp) {
                        is_contig = is_contiguous(ca[i].u.s.data_mask) &&
                                is_contiguous(ca[i].u.s.code_mask);
                } else {
                        is_contig = is_contiguous(ca[i].u.ways_mask);
                }
                if (!is_contig) {
                        LOG_ERROR("L3 COS%u bit mask is not contiguous!\n",
                                  ca[i].class_id);
                        _pqos_api_unlock();
                        return PQOS_RETVAL_PARAM;
                }
        }

        ASSERT(m_cap != NULL);
        ret = pqos_l3ca_get_cos_num(m_cap, &count);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return ret;             /**< perhaps no L3CA capability */
        }

        if (num_ca > count) {
                _pqos_api_unlock();
                return PQOS_RETVAL_ERROR;
        }

        ret = pqos_l3ca_cdp_enabled(m_cap, NULL, &cdp_enabled);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return ret;
        }

        ASSERT(m_cpu != NULL);
        ret = pqos_cpu_get_cores(m_cpu, socket, 1, &count, &core);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return ret;
        }

        if (cdp_enabled) {
                for (i = 0; i < num_ca; i++) {
                        uint32_t reg =
                                (ca[i].class_id*2) + PQOS_MSR_L3CA_MASK_START;
                        int retval = MACHINE_RETVAL_OK;
                        uint64_t cmask = 0, dmask = 0;

                        if (ca[i].cdp) {
                                dmask = ca[i].u.s.data_mask;
                                cmask = ca[i].u.s.code_mask;
                        } else {
                                dmask = ca[i].u.ways_mask;
                                cmask = ca[i].u.ways_mask;
                        }

                        retval = msr_write(core, reg, dmask);
                        if (retval != MACHINE_RETVAL_OK) {
                                _pqos_api_unlock();
                                return PQOS_RETVAL_ERROR;
                        }

                        retval = msr_write(core, reg+1, cmask);
                        if (retval != MACHINE_RETVAL_OK) {
                                _pqos_api_unlock();
                                return PQOS_RETVAL_ERROR;
                        }
                }
        } else {
                for (i = 0; i < num_ca; i++) {
                        uint32_t reg =
                                ca[i].class_id + PQOS_MSR_L3CA_MASK_START;
                        uint64_t val = ca[i].u.ways_mask;
                        int retval = MACHINE_RETVAL_OK;

                        if (ca[i].cdp) {
                                LOG_ERROR("Attempting to set CDP COS "
                                          "while CDP is disabled!\n");
                                _pqos_api_unlock();
                                return PQOS_RETVAL_ERROR;
                        }

                        retval = msr_write(core, reg, val);
                        if (retval != MACHINE_RETVAL_OK) {
                                _pqos_api_unlock();
                                return PQOS_RETVAL_ERROR;
                        }
                }
        }

        _pqos_api_unlock();
        return ret;
}

int
pqos_l3ca_get(const unsigned socket,
              const unsigned max_num_ca,
              unsigned *num_ca,
              struct pqos_l3ca *ca)
{
        int ret = PQOS_RETVAL_OK;
        unsigned i = 0, count = 0,
                core = 0, core_count = 0;
        uint32_t reg = 0;
        uint64_t val = 0;
        int retval = MACHINE_RETVAL_OK;
        int cdp_enabled = 0;

        _pqos_api_lock();

        ret = _pqos_check_init(1);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return ret;
        }

        if (num_ca == NULL || ca == NULL || max_num_ca == 0) {
                _pqos_api_unlock();
                return PQOS_RETVAL_PARAM;
        }

        ASSERT(m_cap != NULL);
        ret = pqos_l3ca_get_cos_num(m_cap, &count);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return ret;             /**< perhaps no L3CA capability */
        }

        ret = pqos_l3ca_cdp_enabled(m_cap, NULL, &cdp_enabled);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return ret;
        }

        if (count > max_num_ca) {
                _pqos_api_unlock();
                return PQOS_RETVAL_ERROR;
        }

        ASSERT(m_cpu != NULL);
        ret = pqos_cpu_get_cores(m_cpu, socket, 1, &core_count, &core);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return ret;
        }
        ASSERT(core_count > 0);

        if (cdp_enabled) {
                for (i = 0, reg = PQOS_MSR_L3CA_MASK_START;
                     i < count; i++, reg += 2) {
                        ca[i].cdp = 1;
                        ca[i].class_id = i;

                        retval = msr_read(core, reg, &val);
                        if (retval != MACHINE_RETVAL_OK) {
                                _pqos_api_unlock();
                                return PQOS_RETVAL_ERROR;
                        }
                        ca[i].u.s.data_mask = val;

                        retval = msr_read(core, reg+1, &val);
                        if (retval != MACHINE_RETVAL_OK) {
                                _pqos_api_unlock();
                                return PQOS_RETVAL_ERROR;
                        }
                        ca[i].u.s.code_mask = val;
                }
        } else {
                for (i = 0, reg = PQOS_MSR_L3CA_MASK_START;
                     i < count; i++, reg++) {
                        retval = msr_read(core, reg, &val);
                        if (retval != MACHINE_RETVAL_OK) {
                                _pqos_api_unlock();
                                return PQOS_RETVAL_ERROR;
                        }
                        ca[i].cdp = 0;
                        ca[i].class_id = i;
                        ca[i].u.ways_mask = val;
                }
        }

        *num_ca = count;

        _pqos_api_unlock();
        return ret;
}

int
pqos_l2ca_set(const unsigned socket,
              const unsigned num_ca,
              const struct pqos_l2ca *ca)
{
        int ret = PQOS_RETVAL_OK;
        unsigned i = 0, count = 0, core = 0;

        _pqos_api_lock();

        ret = _pqos_check_init(1);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return ret;
        }

        if (ca == NULL || num_ca == 0) {
                _pqos_api_unlock();
                return PQOS_RETVAL_PARAM;
        }

        /**
         * Check if L2 CAT is supported
         */
        ASSERT(m_cap != NULL);
        ret = pqos_l2ca_get_cos_num(m_cap, &count);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return PQOS_RETVAL_RESOURCE; /* L2 CAT not supported */
        }

        /**
         * Check if class bitmasks are contiguous and
         * if class id's are within allowed range.
         */
        for (i = 0; i < num_ca; i++) {
                if (!is_contiguous(ca[i].ways_mask)) {
                        LOG_ERROR("L2 COS%u bit mask is not contiguous!\n",
                                  ca[i].class_id);
                        _pqos_api_unlock();
                        return PQOS_RETVAL_PARAM;
                }
                if (ca[i].class_id >= count) {
                        LOG_ERROR("L2 COS%u is out of range (COS%u is max)!\n",
                                  ca[i].class_id, count - 1);
                        _pqos_api_unlock();
                        return PQOS_RETVAL_PARAM;
                }
        }

        /**
         * Pick one core from the socket and
         * perfrom MSR writes to COS registers on the socket.
         */
        ASSERT(m_cpu != NULL);
        ret = pqos_cpu_get_cores(m_cpu, socket, 1, &count, &core);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return ret;
        }

        for (i = 0; i < num_ca; i++) {
                uint32_t reg = ca[i].class_id + PQOS_MSR_L2CA_MASK_START;
                uint64_t val = ca[i].ways_mask;
                int retval = MACHINE_RETVAL_OK;

                retval = msr_write(core, reg, val);
                if (retval != MACHINE_RETVAL_OK) {
                        _pqos_api_unlock();
                        return PQOS_RETVAL_ERROR;
                }
        }

        _pqos_api_unlock();
        return ret;
}

int
pqos_l2ca_get(const unsigned socket,
              const unsigned max_num_ca,
              unsigned *num_ca,
              struct pqos_l2ca *ca)
{
        int ret = PQOS_RETVAL_OK;
        unsigned i = 0, count = 0;
        unsigned core = 0, core_count = 0;

        _pqos_api_lock();

        ret = _pqos_check_init(1);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return ret;
        }

        if (num_ca == NULL || ca == NULL || max_num_ca == 0) {
                _pqos_api_unlock();
                return PQOS_RETVAL_PARAM;
        }

        ASSERT(m_cap != NULL);
        ret = pqos_l2ca_get_cos_num(m_cap, &count);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return PQOS_RETVAL_RESOURCE; /* L2 CAT not supported */
        }

        if (max_num_ca < count) {
                /* Not enough space to store the classes */
                _pqos_api_unlock();
                return PQOS_RETVAL_PARAM;
        }

        ASSERT(m_cpu != NULL);
        ret = pqos_cpu_get_cores(m_cpu, socket, 1, &core_count, &core);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return ret;
        }
        ASSERT(core_count > 0);

        for (i = 0; i < count; i++) {
                uint32_t reg = PQOS_MSR_L2CA_MASK_START + i;
                uint64_t val = 0;
                int retval = msr_read(core, reg, &val);

                if (retval != MACHINE_RETVAL_OK) {
                        _pqos_api_unlock();
                        return PQOS_RETVAL_ERROR;
                }
                ca[i].class_id = i;
                ca[i].ways_mask = val;
        }

        *num_ca = count;
        _pqos_api_unlock();
        return ret;
}

int
pqos_alloc_assoc_set(const unsigned lcore,
                     const unsigned class_id)
{
        int ret = PQOS_RETVAL_OK;
        unsigned num_l2_cos = 0, num_l3_cos = 0;
        const uint32_t reg = PQOS_MSR_ASSOC;
        uint64_t val = 0;

        _pqos_api_lock();

        ret = _pqos_check_init(1);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return ret;
        }

        ASSERT(m_cpu != NULL);
        ret = pqos_cpu_check_core(m_cpu, lcore);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return PQOS_RETVAL_PARAM;
        }

        ASSERT(m_cap != NULL);
        ret = pqos_l3ca_get_cos_num(m_cap, &num_l3_cos);
        if (ret != PQOS_RETVAL_OK && ret != PQOS_RETVAL_RESOURCE) {
                _pqos_api_unlock();
                return ret;
        }

        ret = pqos_l2ca_get_cos_num(m_cap, &num_l2_cos);
        if (ret != PQOS_RETVAL_OK && ret != PQOS_RETVAL_RESOURCE) {
                _pqos_api_unlock();
                return ret;
        }

        if (class_id > num_l3_cos && class_id > num_l2_cos) {
                /* class_id is out of bounds */
                _pqos_api_unlock();
                return PQOS_RETVAL_PARAM;
        }

        ret = msr_read(lcore, reg, &val);
        if (ret != MACHINE_RETVAL_OK) {
                _pqos_api_unlock();
                return PQOS_RETVAL_ERROR;
        }

        val &= (~PQOS_MSR_ASSOC_QECOS_MASK);
        val |= (((uint64_t) class_id) << PQOS_MSR_ASSOC_QECOS_SHIFT);

        ret = msr_write(lcore, reg, val);
        if (ret != MACHINE_RETVAL_OK) {
                _pqos_api_unlock();
                return PQOS_RETVAL_ERROR;
        }

        _pqos_api_unlock();
        return PQOS_RETVAL_OK;
}

int
pqos_alloc_assoc_get(const unsigned lcore,
                     unsigned *class_id)
{
        const struct pqos_capability *l3_cap = NULL;
        const struct pqos_capability *l2_cap = NULL;
        int ret = PQOS_RETVAL_OK;
        const uint32_t reg = PQOS_MSR_ASSOC;
        uint64_t val = 0;

        _pqos_api_lock();

        ret = _pqos_check_init(1);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return ret;
        }

        if (class_id == NULL) {
                _pqos_api_unlock();
                return PQOS_RETVAL_PARAM;
        }

        ASSERT(m_cpu != NULL);
        ret = pqos_cpu_check_core(m_cpu, lcore);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return PQOS_RETVAL_PARAM;
        }

        ASSERT(m_cap != NULL);
        ret = pqos_cap_get_type(m_cap, PQOS_CAP_TYPE_L3CA, &l3_cap);
        if (ret != PQOS_RETVAL_OK && ret != PQOS_RETVAL_RESOURCE) {
                _pqos_api_unlock();
                return ret;
        }

        ret = pqos_cap_get_type(m_cap, PQOS_CAP_TYPE_L2CA, &l2_cap);
        if (ret != PQOS_RETVAL_OK && ret != PQOS_RETVAL_RESOURCE) {
                _pqos_api_unlock();
                return ret;
        }

        if (l2_cap == NULL && l3_cap == NULL) {
                /* no L2/L3 CAT detected */
                _pqos_api_unlock();
                return PQOS_RETVAL_RESOURCE;
        }

        if (msr_read(lcore, reg, &val) != MACHINE_RETVAL_OK) {
                _pqos_api_unlock();
                return PQOS_RETVAL_ERROR;
        }

        val >>= PQOS_MSR_ASSOC_QECOS_SHIFT;
        *class_id = (unsigned) val;

        _pqos_api_unlock();
        return PQOS_RETVAL_OK;
}

/**
 * @brief Enables or disables CDP across selected CPU sockets
 *
 * @param [in] sockets_num dimension of \a sockets array
 * @param [in] sockets array with socket ids to change CDP config on
 * @param [in] enable CDP enable/disable flag, 1 - enable, 0 - disable
 *
 * @return Operations status
 * @retval PQOS_RETVAL_OK on success
 * @retval PQOS_RETVAL_ERROR on failure, MSR read/write error
 */
static int
cdp_enable(const unsigned sockets_num,
           const unsigned *sockets,
           const int enable)
{
        unsigned j = 0;

        ASSERT(socket_num > 0 && sockets != NULL);

        LOG_INFO("%s CDP across sockets...\n",
                 (enable) ? "Enabling" : "Disabling");

        for (j = 0; j < sockets_num; j++) {
                uint64_t reg = 0;
                unsigned core = 0, count = 0;
                int ret = PQOS_RETVAL_OK;

                ret = pqos_cpu_get_cores(m_cpu, sockets[j], 1, &count, &core);
                if (ret != PQOS_RETVAL_OK)
                        return ret;

                ret = msr_read(core, PQOS_MSR_L3_QOS_CFG, &reg);
                if (ret != MACHINE_RETVAL_OK)
                        return PQOS_RETVAL_ERROR;

                if (enable)
                        reg |= PQOS_MSR_L3_QOS_CFG_CDP_EN;
                else
                        reg &= ~PQOS_MSR_L3_QOS_CFG_CDP_EN;

                ret = msr_write(core, PQOS_MSR_L3_QOS_CFG, reg);
                if (ret != MACHINE_RETVAL_OK)
                        return PQOS_RETVAL_ERROR;
        }

        return PQOS_RETVAL_OK;
}

/**
 * @brief Writes range of CAT COS MSR's with \a msr_val value
 *
 * Used as part of CAT reset process.
 *
 * @param [in] msr_start First MSR to be written
 * @param [in] msr_num Number of MSR's to be written
 * @param [in] coreid Core ID to be used for MSR write operations
 * @param [in] msr_val Value to be written to MSR's
 *
 * @return Operation status
 * @retval PQOS_RETVAL_OK on success
 * @retval PQOS_RETVAL_ERROR on MSR write error
 */
static int
cat_cos_reset(const unsigned msr_start,
              const unsigned msr_num,
              const unsigned coreid,
              const uint64_t msr_val)
{
        int ret = PQOS_RETVAL_OK;
        unsigned i;

        for (i = 0; i < msr_num; i++) {
                int retval = msr_write(coreid, msr_start + i, msr_val);

                if (retval != MACHINE_RETVAL_OK)
                        ret = PQOS_RETVAL_ERROR;
        }

        return ret;
}

/**
 * @brief Associates each of the cores with COS0
 *
 * Operates on m_cpu structure.
 *
 * @return Operation status
 * @retval PQOS_RETVAL_OK on success
 * @retval PQOS_RETVAL_ERROR on MSR write error
 */
static int
cat_assoc_reset(void)
{
        int ret = PQOS_RETVAL_OK;
        unsigned i;

        for (i = 0; i < m_cpu->num_cores; i++) {
                const unsigned class_id = 0;
                const uint32_t reg = PQOS_MSR_ASSOC;
                uint64_t val = 0;
                int retval = MACHINE_RETVAL_OK;

                retval = msr_read(m_cpu->cores[i].lcore, reg, &val);
                if (retval != MACHINE_RETVAL_OK) {
			ret = PQOS_RETVAL_ERROR;
                        continue;
		}

                val &= (~PQOS_MSR_ASSOC_QECOS_MASK);
                val |= (((uint64_t) class_id) << PQOS_MSR_ASSOC_QECOS_SHIFT);

                retval = msr_write(m_cpu->cores[i].lcore, reg, val);
                if (retval != MACHINE_RETVAL_OK) {
			ret = PQOS_RETVAL_ERROR;
                        continue;
		}
        }

        return ret;
}

int
pqos_alloc_reset(const enum pqos_cdp_config l3_cdp_cfg)
{
        unsigned *sockets = NULL;
        unsigned sockets_num = 0, sockets_count = 0;
        const struct pqos_capability *cat_cap = NULL;
        const struct pqos_cap_l3ca *l3_cap = NULL;
        const struct pqos_cap_l2ca *l2_cap = NULL;
        int ret = PQOS_RETVAL_OK;
        unsigned max_l3_cos = 0;
        unsigned j;

        if (l3_cdp_cfg != PQOS_REQUIRE_CDP_ON &&
            l3_cdp_cfg != PQOS_REQUIRE_CDP_OFF &&
            l3_cdp_cfg != PQOS_REQUIRE_CDP_ANY) {
                LOG_ERROR("Unrecognized L3 CDP configuration %d!\n",
                          l3_cdp_cfg);
                return PQOS_RETVAL_PARAM;
        }

        _pqos_api_lock();

        ret = _pqos_check_init(1);
        if (ret != PQOS_RETVAL_OK) {
                _pqos_api_unlock();
                return ret;
        }

        /* Get L3 CAT capabilities */
        (void) pqos_cap_get_type(m_cap, PQOS_CAP_TYPE_L3CA, &cat_cap);
        if (cat_cap != NULL)
                l3_cap = cat_cap->u.l3ca;

        /* Get L2 CAT capabilities */
        cat_cap = NULL;
        (void) pqos_cap_get_type(m_cap, PQOS_CAP_TYPE_L2CA, &cat_cap);
        if (cat_cap != NULL)
                l2_cap = cat_cap->u.l2ca;

        /* Check if either L2 or L3 CAT is supported */
        if (l2_cap == NULL && l3_cap == NULL) {
                ret = PQOS_RETVAL_RESOURCE; /* no L2/L3 CAT present */
                goto pqos_alloc_reset_exit;
        }
        if (l3_cap != NULL) {
                /* Check against erroneous CDP request */
                if (l3_cdp_cfg == PQOS_REQUIRE_CDP_ON && !l3_cap->cdp) {
                        LOG_ERROR("CAT/CDP requested but not supported by the "
                                  "platform!\n");
                        ret = PQOS_RETVAL_PARAM;
                        goto pqos_alloc_reset_exit;
                }

                /* Get maximum number of L3 CAT classes */
                max_l3_cos = l3_cap->num_classes;
                if (l3_cap->cdp && l3_cap->cdp_on)
                        max_l3_cos = max_l3_cos * 2;
        }

        /**
         * Get number & list of sockets in the system
         */
        ret = pqos_cpu_get_num_sockets(m_cpu, &sockets_count);
        if (ret != PQOS_RETVAL_OK)
                goto pqos_alloc_reset_exit;

	sockets = malloc(sizeof(sockets[0]) * sockets_count);
	if (sockets == NULL) {
		ret = PQOS_RETVAL_RESOURCE;
                goto pqos_alloc_reset_exit;
        }
        ret = pqos_cpu_get_sockets(m_cpu, sockets_count,
				   &sockets_num, sockets);
        if (ret != PQOS_RETVAL_OK)
                goto pqos_alloc_reset_exit;

        if (sockets_num != sockets_count || sockets_num == 0) {
                ret = PQOS_RETVAL_ERROR;
                goto pqos_alloc_reset_exit;
        }

        /**
         * Change COS definition on all sockets
         * so that each COS allows for access to all cache ways
         */
        for (j = 0; j < sockets_num; j++) {
                unsigned core = 0, count = 0;

                ret = pqos_cpu_get_cores(m_cpu, sockets[j], 1, &count, &core);
                if (ret != PQOS_RETVAL_OK)
                        goto pqos_alloc_reset_exit;

                if (l3_cap != NULL) {
                        const uint64_t ways_mask =
                                (1ULL << l3_cap->num_ways) - 1ULL;

                        ret = cat_cos_reset(PQOS_MSR_L3CA_MASK_START,
                                            max_l3_cos, core, ways_mask);
                        if (ret != PQOS_RETVAL_OK)
                                goto pqos_alloc_reset_exit;
                }

                if (l2_cap != NULL) {
                        const uint64_t ways_mask =
                                (1ULL << l2_cap->num_ways) - 1ULL;

                        ret = cat_cos_reset(PQOS_MSR_L2CA_MASK_START,
                                            l2_cap->num_classes, core,
                                            ways_mask);
                        if (ret != PQOS_RETVAL_OK)
                                goto pqos_alloc_reset_exit;
                }
        }

        /**
         * Associate all cores with COS0
         */
        ret = cat_assoc_reset();
        if (ret != PQOS_RETVAL_OK)
                goto pqos_alloc_reset_exit;

        /**
         * Turn L3 CDP ON or OFF upon the request
         */
        if (l3_cap != NULL) {
                if (l3_cdp_cfg == PQOS_REQUIRE_CDP_ON && !l3_cap->cdp_on) {
                        /**
                         * Turn on CDP
                         */
                        LOG_INFO("Turning CDP ON ...\n");
                        ret = cdp_enable(sockets_num, sockets, 1);
                        if (ret != PQOS_RETVAL_OK) {
                                LOG_ERROR("CDP enable error!\n");
                                goto pqos_alloc_reset_exit;
                        }
                        _pqos_cap_l3cdp_change(l3_cap->cdp_on, 1);
                }

                if (l3_cdp_cfg == PQOS_REQUIRE_CDP_OFF &&
                    l3_cap->cdp_on && l3_cap->cdp) {
                        /**
                         * Turn off CDP
                         */
                        LOG_INFO("Turning CDP OFF ...\n");
                        ret = cdp_enable(sockets_num, sockets, 0);
                        if (ret != PQOS_RETVAL_OK) {
                                LOG_ERROR("CDP disable error!\n");
                                goto pqos_alloc_reset_exit;
                        }
                        _pqos_cap_l3cdp_change(l3_cap->cdp_on, 0);
                }
        }

 pqos_alloc_reset_exit:
        if (sockets != NULL)
                free(sockets);
        _pqos_api_unlock();
        return ret;
}
