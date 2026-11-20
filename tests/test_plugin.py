#!/usr/bin/env python3
"""The Shift Light menu (plugin/ShiftLight.xml, ShiftLight.manifest) against src/slplugin_ids.h and src/slplugin.cpp:
every raAction is registered by the plugin, every component's action and model exists, the radio buttons are the shift
points the plugin writes, and the connector id is not one of the apps already on the car.

    python3 -I tests/test_plugin.py plugin/ShiftLight.xml plugin/ShiftLight.manifest src/slplugin_ids.h src/slplugin.cpp
"""
import re
import sys
import xml.etree.ElementTree as ET

fails = checks = 0


def check(ok, msg):
    global fails, checks
    checks += 1
    if not ok:
        fails += 1
        print("FAIL", msg)


def main(xml_path, manifest_path, ids_path, cpp_path):
    ids = {k: v for k, v in re.findall(r"#define (SLP_\w+) (\d+)", open(ids_path).read())}
    ids = {k: int(v) for k, v in ids.items()}
    cpp = open(cpp_path).read()

    root = ET.parse(xml_path).getroot()
    app = root.find("pluginApp")
    check(app is not None and app.get("applicationType") == "VehicleInformation", "a My Vehicle app")
    actions = {int(a.get("id")): a for a in app.find("actions")}
    ra = {i for i, a in actions.items() if a.tag == "raAction"}
    hmi = {i: int(a.get("target")) for i, a in actions.items() if a.tag == "hmiAction"}
    models = {int(m.get("id")): m.tag for m in app.find("models")}
    states = {int(s.get("id")): s for s in app.find("hmiStates")}

    entry = app.find("entryButton")
    check(int(entry.get("action")) in hmi and hmi[int(entry.get("action"))] in states, "the entry opens a state")
    for i, target in hmi.items():
        check(target in states, f"hmiAction {i} targets state {target}, which exists")
    comp_ids = []
    for s in states.values():
        for c in s.find("components"):
            comp_ids.append(int(c.get("id")))
            a = c.get("action")
            check(a is not None and int(a) in actions, f"component {c.get('id')}: its action {a} exists")
            m = c.get("model")
            if c.tag in ("checkbox", "radiobutton"):
                check(m is not None and models.get(int(m)) == "raBoolModel", f"component {c.get('id')}: bool model {m}")
                check(int(a) in ra, f"component {c.get('id')}: a checkbox or radio button needs a raAction")
            check(bool(c.get("text")), f"component {c.get('id')} has a text")
    check(len(comp_ids) == len(set(comp_ids)), "component ids are unique")

    # the plugin registers exactly the raActions
    want = {ids["SLP_ACT_ON"], ids["SLP_ACT_SPORT"]} | {ids["SLP_ACT_RPM0"] + k for k in range(ids["SLP_RPM_N"])}
    check(ra == want, f"raActions in the XML {sorted(ra)} = handled by the plugin {sorted(want)}")
    for name in ("SLP_ACT_ON", "SLP_ACT_SPORT", "SLP_ACT_RPM0"):
        check(re.search(r"addAction\(self, " + name + r"\b", cpp) is not None, f"slplugin.cpp registers {name}")
    check(re.search(r"addAction\(self, SLP_ACT_MANUAL\b", cpp) is None and ids["SLP_ACT_MANUAL"] not in actions,
          "no manual-mode row (this car reports no gearbox mode)")
    for name in ("SLP_MODEL_ON", "SLP_MODEL_SPORT"):
        check(models.get(ids[name]) == "raBoolModel", f"{name} {ids[name]} is a raBoolModel in the XML")
        check(re.search(r"updateModel\(" + name + r"\b", cpp) is not None, f"slplugin.cpp updates {name}")
    check(hmi.get(ids["SLP_ACT_POINT"]) == 200, "the Shift point row opens state 200")

    # the radio buttons: model RPM0+k, action RPM0+k, text = the shift point
    radios = [c for c in states[200].find("components") if c.tag == "radiobutton"]
    check(len(radios) == ids["SLP_RPM_N"], f"{len(radios)} radio buttons")
    for k, c in enumerate(radios):
        rpm = ids["SLP_RPM_FIRST"] + k * ids["SLP_RPM_STEP"]
        check(int(c.get("action")) == ids["SLP_ACT_RPM0"] + k and int(c.get("model")) == ids["SLP_MODEL_RPM0"] + k,
              f"radio {k}: action and model")
        check(c.get("text") == f"{rpm:,} rpm", f"radio {k}: text {c.get('text')!r}, want {rpm:,} rpm")
    check(ids["SLP_RPM_FIRST"] <= ids["SLP_RPM_DEFAULT"] < ids["SLP_RPM_FIRST"] + ids["SLP_RPM_N"] * ids["SLP_RPM_STEP"]
          and (ids["SLP_RPM_DEFAULT"] - ids["SLP_RPM_FIRST"]) % ids["SLP_RPM_STEP"] == 0, "the default is a choice")

    # the manifest
    man = ET.parse(manifest_path).getroot()
    check(man.findtext("name") == "ShiftLight" and man.findtext("file") == "{pluginDir}/ShiftLight", "manifest name/file")
    check(man.find("config").findtext("autoStart") == "yes", "autoStart")
    cid = int(man.find(".//pluginAppConnector").findtext("id"))
    on_car = {100, 102, 104, 107, 201, 217, 218}  # the car's Plugins, 2026-11-19
    check(cid not in on_car, f"connector id {cid} is not used on the car ({sorted(on_car)})")
    check(man.find(".//DescFile").text == "{pluginDir}/ShiftLight.xml", "DescFile")

    # the plugin's exported entry and its library name
    check('"Shift Light"' in cpp and "pluginAppInfo" in cpp, "pluginAppInfo")
    print(f"{checks} checks, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:5]))
