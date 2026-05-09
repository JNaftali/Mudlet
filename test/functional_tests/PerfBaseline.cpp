/***************************************************************************
 *   Copyright (C) 2026 by Mudlet Developers                               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

/*
 * Perf-baseline harness for the libmudlet split (issue #9011).
 *
 * Drives a fixed synthetic MUD workload through the real
 *   cTelnet::processSocketData -> TBuffer::translateToPlainText
 * pipeline and reports three metrics in a stable, machine-parseable form:
 *
 *   - lines/sec   throughput at three workload sizes (small/medium/large)
 *   - MB/sec      same workload by bytes
 *   - peak RSS    process-wide peak resident set, before vs. after the workload
 *
 * Methodology: one discarded warmup pass per workload to absorb cold-cache
 * cost, then kIterations timed runs; reported number is the median elapsed
 * time. Median is more robust to scheduler jitter than mean.
 *
 * Output is plain key=value on stdout, one per line, between
 * `=== PerfBaseline begin ===` and `=== PerfBaseline end ===` markers.
 * No QVERIFY-against-fixed-numbers — the test passes as long as the
 * pipeline completes. Numbers are recorded; baseline comparison is a
 * follow-up PR once we have stable hardware to commit numbers from.
 *
 * Run with: ctest -R PerfBaseline -V
 *           (-V is required to see the metric output)
 *
 * RSS interpretation:
 *   peak_rss_kb_after includes Mudlet's scrollback accumulation across
 *   the entire benchmark run. TBuffer keeps every received line in
 *   scrollback by design (~3.6 KB/line at steady state, much higher with
 *   ASAN). The diagnostic [diag] stamps below the metrics show RSS and
 *   TBuffer line count by phase so this growth is visible.
 *
 *   For per-PR regression detection the more discriminating number is
 *   rss_kb_per_accumulated_line: this ratio should stay stable across
 *   libmudlet-split PRs that touch the telnet -> TBuffer path. A
 *   regression > 10% in the ratio means the migrated code path stores
 *   more per line, distinct from "TBuffer happened to accumulate more
 *   lines because the workload changed."
 *
 * Caveats:
 *   - Builds with the project's default sanitizers (ASAN on Linux/macOS).
 *     Numbers are inflated; comparisons across libmudlet-split PRs are
 *     still meaningful because both sides see the same overhead.
 *   - Telnet→TBuffer pipeline only. A Lua-driven trigger workload that
 *     exercises the scripting hot path is left for a follow-up PR.
 */

#include <QtTest/QtTest>

#include <cstdio>
#include <cstdlib>

#if defined(Q_OS_UNIX)
#include <sys/resource.h>
#endif
#if defined(Q_OS_WINDOWS)
#include <windows.h>
#include <psapi.h>
#endif

#include "Host.h"
#include "MudletInstanceCoordinator.h"
#include "TBuffer.h"
#include "TConsole.h"
#include "TMainConsole.h"
#include "TelnetServerStub.h"
#include "ctelnet.h"
#include "dlgConnectionProfiles.h"
#include "mudlet.h"

extern void qInitResources_mudlet();
extern void qInitResources_qm();
extern void qInitResources_additional_splash_screens();
extern void qInitResources_mudlet_fonts_common();
extern void qInitResources_mudlet_fonts_posix();

namespace {

void initializeResources()
{
#ifdef INCLUDE_VARIABLE_SPLASH_SCREEN
    qInitResources_additional_splash_screens();
#endif
#ifdef INCLUDE_FONTS
    qInitResources_mudlet_fonts_common();
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    qInitResources_mudlet_fonts_posix();
#endif
#endif
    qInitResources_mudlet();
    qInitResources_qm();
}

// Peak resident set in KiB, or -1 if unavailable on this platform.
int64_t peakRssKb()
{
#if defined(Q_OS_UNIX)
    struct rusage ru = {};
    if (getrusage(RUSAGE_SELF, &ru) != 0) {
        return -1;
    }
#if defined(Q_OS_MACOS)
    // ru_maxrss is bytes on macOS.
    return static_cast<int64_t>(ru.ru_maxrss / 1024);
#else
    // Linux and most BSDs report KiB.
    return static_cast<int64_t>(ru.ru_maxrss);
#endif
#elif defined(Q_OS_WINDOWS)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<int64_t>(pmc.PeakWorkingSetSize / 1024);
    }
    return -1;
#else
    return -1;
#endif
}

QByteArray generateMudTraffic(int lines)
{
    // Same shape as TelnetBenchmark: short ANSI-coloured lines plus an
    // occasional prompt with telnet IAC GA. Representative of typical
    // MUD output. Average ~110 bytes/line.
    QByteArray out;
    out.reserve(lines * 120);
    for (int i = 0; i < lines; ++i) {
        out.append("\x1b[1;32m");
        out.append("You are standing in a dark forest. The trees tower above you. ");
        out.append("\x1b[0m\r\n");
        if (i % 5 == 0) {
            out.append("\x1b[1;37m> \x1b[0m\xff\xf9");
        }
    }
    return out;
}

} // namespace

class PerfBaseline : public QObject
{
    Q_OBJECT

private:
    TelnetServerStub* mpServer = nullptr;
    Host* mpHost = nullptr;
    const QString mHostname = qsl("PerfBaseline-Host");
    const QString mPort = qsl("4002");
    const QString mLocalhost = qsl("localhost");

    void startProfile()
    {
        QTimer::singleShot(0, qApp, [this]() {
            mudlet::self()->startAutoLogin({});
            QTest::qWait(100);
            QTest::mouseClick(mudlet::self()->mpConnectionDialog->new_profile_button, Qt::LeftButton);
            QTest::qWait(100);
            QTest::keyClicks(QApplication::focusWidget(), mHostname);
            QTest::qWait(100);
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
            QTest::qWait(100);
            QTest::keyClicks(QApplication::focusWidget(), mLocalhost);
            QTest::qWait(100);
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
            QTest::qWait(100);
            QTest::keyClicks(QApplication::focusWidget(), mPort);
            QTest::qWait(100);
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        });

        QSignalSpy loadedSpy(mudlet::self(), &mudlet::signal_profileLoaded);
        if (!loadedSpy.wait(1000)) {
            QFAIL("Profile took too long to load.");
        }
        mpHost = mudlet::self()->getActiveHost();
        QVERIFY(mpHost != nullptr);

        QSignalSpy connectedSpy(&(mpHost->mTelnet), &cTelnet::signal_connected);
        if (!connectedSpy.wait(500)) {
            QFAIL("Could not connect to TelnetServerStub.");
        }
    }

    void deleteProfileDirectory()
    {
        const QString path = mudlet::getMudletPath(enums::profileHomePath, mHostname);
        QDir dir(path);
        if (dir.exists()) {
            dir.removeRecursively();
        }
    }

private slots:
    void initTestCase() { initializeResources(); }

    void init()
    {
        mpServer = new TelnetServerStub(qApp);
        mpServer->start(mLocalhost, mPort.toUShort());
        mudlet::start();
        mudlet::self()->setupConfig();
        mudlet::self()->takeOwnershipOfInstanceCoordinator(std::make_unique<MudletInstanceCoordinator>("MudletInstanceCoordinator"));
        mudlet::self()->init();
        mudlet::self()->setStorePasswordsSecurely(false);
        deleteProfileDirectory();
        startProfile();
    }

    void cleanup()
    {
        mpHost = nullptr;
        delete mpServer;
        mpServer = nullptr;
        deleteProfileDirectory();
        delete mudlet::self();
    }

    void measureBaseline()
    {
        QVERIFY(mpHost != nullptr);

        struct Workload
        {
            const char* name;
            int lines;
        };
        const Workload workloads[] = {
                {"small", 100},
                {"medium", 1000},
                {"large", 10000},
        };

        constexpr int kIterations = 5;

        auto lineCount = [this]() -> int {
            return mpHost->mpConsole ? mpHost->mpConsole->buffer.size() : 0;
        };
        auto stamp = [&](const char* phase) {
            std::printf("[diag] %-32s rss_kb=%-10lld lines=%d\n", phase, static_cast<long long>(peakRssKb()), lineCount());
            std::fflush(stdout);
        };

        stamp("before any workload");

        // Sample once after init has stabilised but before any workload runs.
        // ru_maxrss tracks process-wide peak monotonically, so the
        // post-workload read is always >= the pre-workload read.
        const int64_t rssBefore = peakRssKb();

        struct Result
        {
            double linesPerSec; // median
            double mbPerSec;    // median
            qint64 medianElapsedNs;
            qint64 minElapsedNs;
            qint64 maxElapsedNs;
            int bytes;
        };
        QHash<QString, Result> results;

        for (const auto& w : workloads) {
            QByteArray data = generateMudTraffic(w.lines);
            const int bytes = data.size();

            // Warmup: discard one run to absorb cold-cache and any
            // first-iteration allocations.
            QTest::qWait(20);
            mpHost->mTelnet.loopbackTest(data);
            stamp(qsl("after %1 warmup").arg(w.name).toUtf8().constData());

            QVector<qint64> samples;
            samples.reserve(kIterations);
            for (int i = 0; i < kIterations; ++i) {
                QTest::qWait(20);
                QElapsedTimer t;
                t.start();
                mpHost->mTelnet.loopbackTest(data);
                samples.append(t.nsecsElapsed());
            }
            stamp(qsl("after %1 timed runs").arg(w.name).toUtf8().constData());
            std::sort(samples.begin(), samples.end());
            const qint64 medianNs = samples[kIterations / 2];
            const double medianSec = medianNs / 1e9;

            results[QString::fromLatin1(w.name)] = Result{
                    w.lines / medianSec,
                    (bytes / 1048576.0) / medianSec,
                    medianNs,
                    samples.first(),
                    samples.last(),
                    bytes,
            };
        }

        const int64_t rssAfter = peakRssKb();
        const int linesAccumulated = lineCount();
        const double rssKbPerLine = linesAccumulated > 0 ? static_cast<double>(rssAfter - rssBefore) / linesAccumulated : 0.0;

        // Stable, machine-parseable output. Anchored between markers so
        // CI scripts can diff just the metric block, ignoring Qt log noise.
        std::printf("=== PerfBaseline begin ===\n");
        std::printf("iterations             = %12d\n", kIterations);
        for (const auto& w : workloads) {
            const auto& r = results[QString::fromLatin1(w.name)];
            std::printf("lines_per_sec_%-7s = %12.0f  (%d lines, median %.3f ms, min %.3f, max %.3f)\n",
                        w.name,
                        r.linesPerSec,
                        w.lines,
                        r.medianElapsedNs / 1e6,
                        r.minElapsedNs / 1e6,
                        r.maxElapsedNs / 1e6);
        }
        for (const auto& w : workloads) {
            const auto& r = results[QString::fromLatin1(w.name)];
            std::printf("mb_per_sec_%-10s = %12.3f  (%d bytes, median %.3f ms)\n", w.name, r.mbPerSec, r.bytes, r.medianElapsedNs / 1e6);
        }
        std::printf("peak_rss_kb_before          = %12lld\n", static_cast<long long>(rssBefore));
        std::printf("peak_rss_kb_after           = %12lld\n", static_cast<long long>(rssAfter));
        std::printf("peak_rss_delta_kb           = %12lld\n", static_cast<long long>(rssAfter - rssBefore));
        std::printf("lines_accumulated           = %12d\n", linesAccumulated);
        std::printf("rss_kb_per_accumulated_line = %12.2f\n", rssKbPerLine);
        std::printf("=== PerfBaseline end ===\n");
        std::fflush(stdout);
    }
};

#include "PerfBaseline.moc"
QTEST_MAIN(PerfBaseline)
