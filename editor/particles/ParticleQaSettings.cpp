// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleQaSettings.h"

using namespace jasperengine::editor;

const std::vector<ParticleQaField>& jasperengine::editor::particleQaFields() {
    using S = ParticleQaSettings;
    static const std::vector<ParticleQaField> fields = {
        {"particles.qa.overdraw_mobile", "overdrawMobile", "Overdraw Layers Mobile", "Peak particle layers over a pixel that warn on mobile; twice as many fail.", &S::overdrawMobile, nullptr, 1, 256},
        {"particles.qa.overdraw_web", "overdrawWeb", "Overdraw Layers Web", "Peak particle layers over a pixel that warn on the web; twice as many fail.", &S::overdrawWeb, nullptr, 1, 256},
        {"particles.qa.overdraw_desktop", "overdrawDesktop", "Overdraw Layers Desktop", "Peak particle layers over a pixel that warn on desktop; twice as many fail.", &S::overdrawDesktop, nullptr, 1, 256},
        {"particles.qa.cpu_ms_mobile", "cpuMsMobile", "CPU Budget Mobile (ms)", "Estimated simulation and render prep one effect may take a frame on mobile.", &S::cpuMsMobile, nullptr, 0.01, 16},
        {"particles.qa.cpu_ms_web", "cpuMsWeb", "CPU Budget Web (ms)", "Estimated simulation and render prep one effect may take a frame on the web.", &S::cpuMsWeb, nullptr, 0.01, 16},
        {"particles.qa.cpu_ms_desktop", "cpuMsDesktop", "CPU Budget Desktop (ms)", "Estimated simulation and render prep one effect may take a frame on desktop.", &S::cpuMsDesktop, nullptr, 0.01, 16},
        {"particles.qa.blown_out_percent", "blownOutPercent", "Blown Out (%)", "Share of the covered pixels clipped to white that warns.", &S::blownOutPercent, nullptr, 1, 100},
        {"particles.qa.invisible_percent", "invisiblePercent", "Invisible Below (%)", "A frame whose particles cover less of the view than this counts as blank.", &S::invisiblePercent, nullptr, 0, 10},
        {"particles.qa.pop_percent", "popPercent", "Pop (%)", "Share of the live particles dying visible in one tick that warns.", &S::popPercent, nullptr, 1, 100},
        {"particles.qa.fix_rounds", "fixRounds", "Fix Rounds", "Consecutive failures of a check after which an agent stops and reports.", nullptr, &S::fixRounds, 1, 10},
    };
    return fields;
}

bool ParticleQaSettings::operator==(const ParticleQaSettings& other) const {
    for (const ParticleQaField& field : particleQaFields()) {
        if (field.real && this->*field.real != other.*field.real) return false;
        if (field.whole && this->*field.whole != other.*field.whole) return false;
    }
    return true;
}
