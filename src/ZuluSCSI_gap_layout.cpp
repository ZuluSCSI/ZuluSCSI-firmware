/**
 * ZuluSCSI™ - Copyright (c) 2026 Rabbit Hole Computing™
 *
 * ZuluSCSI™ firmware is licensed under the GPL version 3 or any later version.
 *
 * https://www.gnu.org/licenses/gpl-3.0.html
 * ----
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 **/

#include "ZuluSCSI_gap_layout.h"

zuluscsi_align_unaligned_t gapLayoutResolveAuto(zuluscsi_align_unaligned_t requestedMode, uint32_t blockSize)
{
    if (requestedMode != ALIGN_UNALIGNED_AUTO)
        return requestedMode;

    if (blockSize == 520)
        return ALIGN_UNALIGNED_CISC;
    else if (blockSize == 522)
        return ALIGN_UNALIGNED_PPC;
    else
        return ALIGN_UNALIGNED_OFF;
}

void gapLayoutNextRun(zuluscsi_align_unaligned_t alignMode, uint32_t blockSize,
                      uint32_t logicalSector, uint32_t sectorsRemaining,
                      gap_layout_run_t *out)
{
    if (alignMode == ALIGN_UNALIGNED_CISC)
    {
        // Every sector owns its own isolated slot -- a run is always
        // exactly one sector.
        out->sectorCount = 1;
        out->unitPhysicalOffset = (uint64_t)logicalSector * GAP_CISC_SLOT_SIZE;
        out->unitPhysicalSize = GAP_CISC_SLOT_SIZE;
        out->byteOffsetInUnit = 0;
        out->unitSectorCapacity = 1;
        out->sectorIndexInUnit = 0;
    }
    else // ALIGN_UNALIGNED_PPC
    {
        uint32_t groupIndex = logicalSector / GAP_PPC_GROUP_SECTORS;
        uint32_t sectorInGroup = logicalSector % GAP_PPC_GROUP_SECTORS;
        uint32_t sectorsLeftInGroup = GAP_PPC_GROUP_SECTORS - sectorInGroup;

        out->sectorCount = sectorsRemaining < sectorsLeftInGroup ? sectorsRemaining : sectorsLeftInGroup;
        out->unitPhysicalOffset = (uint64_t)groupIndex * GAP_PPC_GROUP_PHYS_SIZE;
        out->unitPhysicalSize = GAP_PPC_GROUP_PHYS_SIZE;
        out->byteOffsetInUnit = sectorInGroup * blockSize;
        out->unitSectorCapacity = GAP_PPC_GROUP_SECTORS;
        out->sectorIndexInUnit = sectorInGroup;
    }

    if (out->sectorCount == 0)
        out->sectorCount = 1; // sectorsRemaining==0 should never be passed in, but never return an empty run
}

uint64_t gapLayoutPhysicalSize(zuluscsi_align_unaligned_t alignMode, uint32_t blockSize, uint32_t sectorCount)
{
    if (alignMode == ALIGN_UNALIGNED_CISC)
    {
        return (uint64_t)sectorCount * GAP_CISC_SLOT_SIZE;
    }
    else if (alignMode == ALIGN_UNALIGNED_PPC)
    {
        uint64_t groupCount = ((uint64_t)sectorCount + GAP_PPC_GROUP_SECTORS - 1) / GAP_PPC_GROUP_SECTORS;
        return groupCount * GAP_PPC_GROUP_PHYS_SIZE;
    }
    else
    {
        return (uint64_t)sectorCount * blockSize;
    }
}

uint32_t gapLayoutLogicalSectorsInPhysicalSize(zuluscsi_align_unaligned_t alignMode, uint32_t blockSize, uint64_t physicalBytes)
{
    if (alignMode == ALIGN_UNALIGNED_CISC)
    {
        return (uint32_t)(physicalBytes / GAP_CISC_SLOT_SIZE);
    }
    else if (alignMode == ALIGN_UNALIGNED_PPC)
    {
        uint64_t fullGroups = physicalBytes / GAP_PPC_GROUP_PHYS_SIZE;
        return (uint32_t)(fullGroups * GAP_PPC_GROUP_SECTORS);
    }
    else
    {
        return (uint32_t)(physicalBytes / blockSize);
    }
}
