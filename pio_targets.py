Import("env")

env.AddCustomTarget(
    name="deploy_all",
    dependencies=None,
    actions=[
        "$PYTHONEXE -m platformio run -e cyd",
        "$PYTHONEXE -m platformio run -e cyd -t buildfs",
        "$PYTHONEXE -m platformio run -e cyd -t uploadfs",
        "$PYTHONEXE -m platformio run -e cyd -t upload",
        "$PYTHONEXE -m platformio device monitor -e cyd",
    ],
    title="CYD: Reset CYD",
    description="Build/upload LittleFS, build/upload firmware, then open serial monitor",
)