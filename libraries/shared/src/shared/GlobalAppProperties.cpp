//
//  Created by Bradley Austin Davis on 2016/11/29
//  Copyright 2013-2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "GlobalAppProperties.h"

namespace hifi { namespace properties {

    const char* CRASHED = "com.isekaivr.crashed";
    const char* STEAM = "com.isekaivr.launchedFromSteam";
    const char* LOGGER = "com.isekaivr.logger";
    const char* OCULUS_STORE = "com.isekaivr.oculusStore";
    const char* STANDALONE = "com.isekaivr.standalone";
    const char* TEST = "com.isekaivr.test";
    const char* TRACING = "com.isekaivr.tracing";
    const char* HMD = "com.isekaivr.hmd";
    const char* APP_LOCAL_DATA_PATH = "com.isekaivr.appLocalDataPath";

    namespace gl {
        const char* BACKEND = "com.isekaivr.gl.backend";
        const char* PRIMARY_CONTEXT = "com.isekaivr.gl.primaryContext";
    }

} }
