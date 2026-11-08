/*
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    Modified by Latenz (Simone Salgaro), Oct 2026: adds RenderJournal::average().

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once
#include "kwin_export.h"

#include <chrono>
#include <optional>

namespace KWin
{

/**
 * The RenderJournal class measures how long it takes to render frames and estimates how
 * long it will take to render the next frame.
 */
class KWIN_EXPORT RenderJournal
{
public:
    explicit RenderJournal();

    void add(std::chrono::nanoseconds renderTime, std::chrono::nanoseconds presentationTimestamp);

    std::chrono::nanoseconds result() const;
    /**
     * The smoothed render time alone, without the variance margin result() adds to
     * stay ahead of a deadline. That margin holds on to a single slow frame for
     * tens of seconds (its time constant is 6 s, and it starts at the whole spike).
     */
    std::chrono::nanoseconds average() const;

private:
    std::chrono::nanoseconds m_result{0};
    std::chrono::nanoseconds m_variance{0};
    std::optional<std::chrono::nanoseconds> m_lastAdd;
};

} // namespace KWin
