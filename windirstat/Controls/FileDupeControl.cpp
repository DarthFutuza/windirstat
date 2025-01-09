// FileDupeControl.cpp - Implementation of FileDupeControl
//
// WinDirStat - Directory Statistics
// Copyright © WinDirStat Team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//

#include "stdafx.h"

#include "WinDirStat.h"
#include "DirStatDoc.h"
#include "ItemDupe.h"
#include "MainFrame.h"
#include "FileDupeView.h"
#include "Localization.h"

#include <execution>
#include <unordered_map>
#include <ranges>
#include <stack>

CFileDupeControl::CFileDupeControl() : CTreeListControl(20, COptions::DupeViewColumnOrder.Ptr(), COptions::DupeViewColumnWidths.Ptr())
{
    m_Singleton = this;
}

bool CFileDupeControl::GetAscendingDefault(const int column)
{
    return column == COL_ITEMDUP_SIZE_PHYSICAL ||
        column == COL_ITEMDUP_SIZE_LOGICAL ||
        column == COL_ITEMDUP_LASTCHANGE;
}

#pragma warning(push)
#pragma warning(disable:26454)
BEGIN_MESSAGE_MAP(CFileDupeControl, CTreeListControl)
    ON_WM_SETFOCUS()
    ON_WM_KEYDOWN()
END_MESSAGE_MAP()
#pragma warning(pop)

CFileDupeControl* CFileDupeControl::m_Singleton = nullptr;

void CFileDupeControl::ProcessDuplicate(CItem * item, BlockingQueue<CItem*>* queue)
{
    if (!COptions::ScanForDuplicates) return;
    if (COptions::SkipDupeDetectionCloudLinks &&
        CReparsePoints::IsCloudLink(item->GetPathLong(), item->GetAttributes()))
    {
        std::unique_lock lock(m_HashTrackerMutex);
        if (m_ShowCloudWarningOnThisScan &&
            AfxMessageBox(Localization::Lookup(IDS_DUPLICATES_WARNING).c_str(), MB_YESNO) == IDNO)
        {
            COptions::SkipDupeDetectionCloudLinksWarning = false;
        }
        m_ShowCloudWarningOnThisScan = false;
        return;
    }

    // Determine which hash applies to this object
    auto& m_HashTracker = item->GetSizeLogical() <= m_PartialBufferSize
        ? m_HashTrackerSmall : m_HashTrackerLarge;

    // Add to size tracker and exit early if first item
    std::unique_lock lock(m_HashTrackerMutex);
    auto & sizeSet = m_SizeTracker[item->GetSizeLogical()];
    sizeSet.emplace_back(item);
    if (sizeSet.size() < 2) return;
    
    std::vector<BYTE> hashForThisItem;
    auto itemsToHash = std::vector(sizeSet);
    for (const ITEMTYPE & hashType : {ITF_PARTHASH, ITF_FULLHASH })
    {
        // Attempt to hash the file partially
        for (auto& itemToHash : itemsToHash)
        {
            if (itemToHash->IsType(hashType) || itemToHash->IsType(ITF_SKIPHASH)) continue;

            // Compute the hash for the file
            lock.unlock();
            auto hash = itemToHash->GetFileHash(hashType == ITF_PARTHASH ? m_PartialBufferSize : 0, queue);
            lock.lock();

            // Skip if not hashable
            if (hash.empty())
            {
                itemToHash->SetType(itemToHash->GetRawType() | ITF_SKIPHASH);
                continue;
            }

            itemToHash->SetType(itemToHash->GetRawType() | hashType);
            if (itemToHash == item) hashForThisItem = hash;

            // Mark as the full being completed as well
            if (itemToHash->GetSizeLogical() <= m_PartialBufferSize)
                itemToHash->SetType(itemToHash->GetRawType() | ITF_FULLHASH);

            // Add hash to tracking queue
            auto & hashVector = m_HashTracker[hash];
            if (std::ranges::find(hashVector, itemToHash) == hashVector.end())
                hashVector.emplace_back(itemToHash);
        }

        // Return if no hash conflicts
        const auto hashesResult = m_HashTracker.find(hashForThisItem) ;
        if (hashesResult == m_HashTracker.end() || hashesResult->second.size() < 2) return;
        itemsToHash = hashesResult->second;
    }

    // Add the hashes to the UI thread
    if (hashForThisItem.empty() || itemsToHash.empty()) return;
    m_HashTrackerMutex.unlock();
    for (std::lock_guard guard(m_NodeTrackerMutex); const auto& itemToAdd : itemsToHash)
    {
        const auto nodeEntry = m_NodeTracker.find(hashForThisItem);
        auto dupeParent = nodeEntry != m_NodeTracker.end() ? nodeEntry->second : nullptr;

        if (dupeParent == nullptr)
        {
            // Create new root item to hold these duplicates
            dupeParent = new CItemDupe(hashForThisItem, itemToAdd->GetSizePhysical(), itemToAdd->GetSizeLogical());
            m_PendingListAdds.emplace_back(nullptr, dupeParent);
            m_NodeTracker.emplace(hashForThisItem, dupeParent);
        }

        // Add new item
        auto& m_HashParentNode = m_ChildTracker[dupeParent];
        if (m_HashParentNode.contains(itemToAdd)) continue;
        const auto dupeChild = new CItemDupe(itemToAdd);
        m_PendingListAdds.emplace_back(dupeParent, dupeChild);
        m_HashParentNode.emplace(itemToAdd);
    }
    m_HashTrackerMutex.lock();
}

void CFileDupeControl::SortItems()
{
    ASSERT(AfxGetThread() != nullptr);

    // Transfer elements to vector so we do not have to hold the lock 
    m_NodeTrackerMutex.lock();
    std::vector<std::pair<CItemDupe*, CItemDupe*>> pendingAdds = m_PendingListAdds;
    m_PendingListAdds.clear();
    m_PendingListAdds.shrink_to_fit();
    m_NodeTrackerMutex.unlock();

    // Add items to the list
    if (!pendingAdds.empty())
    {
        SetRedraw(FALSE);
        const auto root = reinterpret_cast<CItemDupe*>(GetItem(0));
        for (const auto& [parent, child] : pendingAdds)
            (parent == nullptr ? root : parent)->AddDupeItemChild(child);
        SetRedraw(TRUE);
    }

    CSortingListControl::SortItems();
}

void CFileDupeControl::RemoveItem(CItem* item)
{
    // Determine which hash applies to this object
    auto& m_HashTracker = item->GetSizeLogical() <= m_PartialBufferSize
        ? m_HashTrackerSmall : m_HashTrackerLarge;

    // Exit immediately if not doing duplicate detector
    if (m_HashTracker.empty() && m_SizeTracker.empty()) return;

    std::stack<CItem*> queue({ item });
    while (!queue.empty())
    {
        const auto& qitem = queue.top();
        queue.pop();
        if (qitem->IsType(IT_FILE))
        {
            // Mark as all files as not being hashed anymore
            std::erase(m_SizeTracker.at(qitem->GetSizeLogical()), qitem);
            qitem->SetType(ITF_PARTHASH | ITF_FULLHASH, false);
        }
        else for (const auto& child : qitem->GetChildren())
        {
            queue.push(child);
        }
    }

    // Remove all unhashed files from hash tracker
    for (auto& hashSet : m_HashTracker | std::views::values)
    {
        // Skip if no matches of the item associated with this hash
        std::erase_if(hashSet, [](const auto& hashItem)
        {
            return !hashItem->IsType(ITF_PARTHASH | ITF_FULLHASH);
        });
    }

    // Pause redrawing for mass node removal
    SetRedraw(FALSE);

    // Cleanup any empty visual nodes in the list
    const auto root = reinterpret_cast<CItemDupe*>(GetItem(0));
    for (auto nodeIter = m_NodeTracker.begin(); nodeIter != m_NodeTracker.end(); ++nodeIter)
    {
        auto& [dupeParentKey, dupeParent] = *nodeIter;

        // Remove from child tracker
        auto& childItems = m_ChildTracker[dupeParent];
        for (auto childItem = childItems.begin(); childItem != childItems.end(); ++childItem)
        {
            // Nothing to do if still marked as hashed
            if ((*childItem)->IsType(ITF_PARTHASH | ITF_FULLHASH)) continue;

            // Remove from child tracker and visual tree
            for (auto& visualChild : dupeParent->GetChildren())
            {
                if (visualChild->GetLinkedItem() != (*childItem)) continue;

                dupeParent->RemoveDupeItemChild(visualChild);
                childItem = childItems.erase(childItem);
                break;
            }

            // Break if already at end of list
            if (childItem == childItems.end()) break;
        }

        // When only one node, left remove last node and parent
        if (dupeParent->GetChildren().size() == 1)
        {
            dupeParent->RemoveDupeItemChild(dupeParent->GetChildren().at(0));
            root->RemoveDupeItemChild(dupeParent);
            nodeIter = m_NodeTracker.erase(nodeIter);
            if (nodeIter == m_NodeTracker.end()) break;
        }
    }

    // Resume redrawing and invalidate to force refresh
    SetRedraw(TRUE);
    Invalidate();

    // Cleanup empty structures
    std::erase_if(m_HashTracker, [](const auto& pair)
    {
        return pair.second.empty();
    });
    std::erase_if(m_SizeTracker, [](const auto& pair)
    {
        return pair.second.empty();
    });
    std::erase_if(m_ChildTracker, [](const auto& pair)
    {
        return pair.second.size() <= 1;
    });
}

void CFileDupeControl::OnItemDoubleClick(const int i)
{
    if (const auto item = reinterpret_cast<const CItem*>(GetItem(i)->GetLinkedItem());
        item != nullptr && item->IsType(IT_FILE))
    {
        CDirStatDoc::OpenItem(item);
    }
    else
    {
        CTreeListControl::OnItemDoubleClick(i);
    }
}

void CFileDupeControl::SetRootItem(CTreeListItem* root)
{
    m_ShowCloudWarningOnThisScan = COptions::SkipDupeDetectionCloudLinksWarning;

    // Cleanup visual list
    CTreeListControl::SetRootItem(root);

    // Cleanup support lists
    m_PendingListAdds.clear();
    m_NodeTracker.clear();
    m_HashTrackerSmall.clear();
    m_HashTrackerLarge.clear();
    m_SizeTracker.clear();
    m_ChildTracker.clear();
}

void CFileDupeControl::OnSetFocus(CWnd* pOldWnd)
{
    CTreeListControl::OnSetFocus(pOldWnd);
    CMainFrame::Get()->SetLogicalFocus(LF_DUPELIST);
}

void CFileDupeControl::OnKeyDown(const UINT nChar, const UINT nRepCnt, const UINT nFlags)
{
    if (nChar == VK_TAB)
    {
        CMainFrame::Get()->MoveFocus(LF_EXTENSIONLIST);
    }
    else if (nChar == VK_ESCAPE)
    {
        CMainFrame::Get()->MoveFocus(LF_NONE);
    }
    CTreeListControl::OnKeyDown(nChar, nRepCnt, nFlags);
}
