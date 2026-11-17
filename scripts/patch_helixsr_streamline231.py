#!/usr/bin/env python3
"""HelixSR-Scaler 2.3.1 Streamline fallback for game-only plugins (sl.dlss.dll).

The upstream's warning is misleading if a game owns an SL plugin absent in
the replacement Streamline 2.14.16 folder. Preserve the original game folder
as a search fallback; never copy game DLLs into the shared Scaler directory.
"""
from pathlib import Path
import argparse
cli=argparse.ArgumentParser()
cli.add_argument("upstream")
root=Path(cli.parse_args().upstream)
p=root/"OptiScaler/hooks/Streamline_Hooks.cpp"
s=p.read_text(encoding="utf-8")
old='''            storage.insert(storage.begin(), localSlPathStr.c_str());
            localPref.pathsToPlugins = storage.data();
            localPref.numPathsToPlugins = (uint32_t) storage.size();

            if (!missingDlls.empty())
            {
                std::string toastMsg = "You are missing the following dlls from the streamline folder:\\n";
                for (const auto& missingDll : missingDlls)
                {
                    toastMsg += "- " + missingDll + "\\n";
                }

                ImGui::InsertNotification({ ImGuiToastType::Warning, 20000, toastMsg.c_str() });
            }'''
new='''            // Preserve the game's own plugins as an explicit fallback if it
            // provided no pathsToPlugins. The game's sl.dlss.dll is often NOT
            // shipped with the Scaler Streamline SDK; do not copy it or hide
            // it by substituting only the new directory.
            std::wstring gamePluginsDir;
            if (storage.empty() && !pluginsDir.empty())
            {
                gamePluginsDir = pluginsDir.wstring();
                storage.push_back(gamePluginsDir.c_str());
            }
            storage.insert(storage.begin(), localSlPathStr.c_str());
            localPref.pathsToPlugins = storage.data();
            localPref.numPathsToPlugins = (uint32_t) storage.size();

            if (!missingDlls.empty())
            {
                // The original folder stays in search paths after the new
                // SDK. This is a diagnostic, not a missing-file error:
                // the original game directory still owns its plugins.
                LOG_INFO("Streamline plugins retained from game folder: {} (newer Scaler SDK preferred)",
                         pluginsDir.string());
            }'''
if s.count(old)!=1:raise SystemExit(f"Unexpected Streamline hkslInit source: {s.count(old)} matches")
s=s.replace(old,new,1)
p.write_text(s,encoding="utf-8")
print("PASS: preserve original Streamline game plugins directory without copying proprietary game DLL")
