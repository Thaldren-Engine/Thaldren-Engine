# 32-bit Windows differential tester that runs routines from the original
# executable in-process. Configure separately with a 32-bit toolchain:
#   cmake -S ta_compat/tools/orig_harness -B build-harness   (from a vcvars32 prompt)
cmake_minimum_required(VERSION 3.24)
project(orig_harness LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "orig_harness must be built as a 32-bit Windows executable")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
add_executable(orig_harness
    orig_harness.cpp
    ${ROOT}/ta_compat/src/crt_strtod.cpp
    ${ROOT}/ta_compat/src/hpi.cpp
    ${ROOT}/ta_compat/src/inflate.cpp
    ${ROOT}/ta_compat/src/tdf.cpp
)
target_include_directories(orig_harness PRIVATE ${ROOT}/ta_compat/include ${ROOT}/engine/sim/include)
target_compile_options(orig_harness PRIVATE /W4 /permissive- /fp:precise)

# Unit script VM: original interpreter vs sim::ScriptInstance on every game script.
add_executable(cob_harness
    cob_harness.cpp
    ${ROOT}/ta_compat/src/cob.cpp
    ${ROOT}/ta_compat/src/crt_strtod.cpp
    ${ROOT}/ta_compat/src/hpi.cpp
    ${ROOT}/ta_compat/src/inflate.cpp
    ${ROOT}/ta_compat/src/tdf.cpp
    ${ROOT}/engine/sim/src/rng.cpp
    ${ROOT}/engine/sim/src/script_vm.cpp
)
target_include_directories(cob_harness PRIVATE ${ROOT}/ta_compat/include ${ROOT}/engine/sim/include)
target_compile_options(cob_harness PRIVATE /W4 /permissive- /fp:precise)

# x87 FSINCOS / rotation emulation check (inline x87 + the original rotation routine).
add_executable(x87_check x87_check.cpp ${ROOT}/engine/sim/src/x87_trig.cpp ${ROOT}/engine/sim/src/x87_math.cpp)
target_include_directories(x87_check PRIVATE ${ROOT}/engine/sim/include)
target_compile_options(x87_check PRIVATE /W4 /permissive- /fp:precise)

# Piece pose: original state builder, piece setters and transform sync vs sim::PoseState.
add_executable(pose_harness
    pose_harness.cpp
    ${ROOT}/ta_compat/src/cob.cpp
    ${ROOT}/ta_compat/src/crt_strtod.cpp
    ${ROOT}/ta_compat/src/featuredefs.cpp
    ${ROOT}/ta_compat/src/gaf.cpp
    ${ROOT}/ta_compat/src/hpi.cpp
    ${ROOT}/ta_compat/src/inflate.cpp
    ${ROOT}/ta_compat/src/model3do.cpp
    ${ROOT}/ta_compat/src/tdf.cpp
    ${ROOT}/ta_compat/src/unitdefs.cpp
    ${ROOT}/ta_compat/src/weapondefs.cpp
    ${ROOT}/engine/sim/src/pose.cpp
    ${ROOT}/engine/sim/src/x87_trig.cpp
)
target_include_directories(pose_harness PRIVATE ${ROOT}/ta_compat/include ${ROOT}/engine/sim/include)
target_compile_options(pose_harness PRIVATE /W4 /permissive- /fp:precise)

# Script-facing unit values (GET/SET/transport callbacks) vs sim::unit_get_value / unit_set_value.
add_executable(unitvalue_harness
    unitvalue_harness.cpp
    ${ROOT}/ta_compat/src/cob.cpp
    ${ROOT}/ta_compat/src/crt_strtod.cpp
    ${ROOT}/ta_compat/src/featuredefs.cpp
    ${ROOT}/ta_compat/src/gaf.cpp
    ${ROOT}/ta_compat/src/hpi.cpp
    ${ROOT}/ta_compat/src/inflate.cpp
    ${ROOT}/ta_compat/src/model3do.cpp
    ${ROOT}/ta_compat/src/tdf.cpp
    ${ROOT}/ta_compat/src/tnt.cpp
    ${ROOT}/ta_compat/src/unitdefs.cpp
    ${ROOT}/ta_compat/src/weapondefs.cpp
    ${ROOT}/engine/sim/src/heightmap.cpp
    ${ROOT}/engine/sim/src/pose.cpp
    ${ROOT}/engine/sim/src/script_vm.cpp
    ${ROOT}/engine/sim/src/unit.cpp
    ${ROOT}/engine/sim/src/x87_math.cpp
    ${ROOT}/engine/sim/src/x87_trig.cpp
)
target_include_directories(unitvalue_harness PRIVATE ${ROOT}/ta_compat/include ${ROOT}/engine/sim/include)
target_compile_options(unitvalue_harness PRIVATE /W4 /permissive- /fp:precise)

# Per-step unit pass with real scripts vs sim::run_unit_pass.
add_executable(unitloop_harness
    unitloop_harness.cpp
    ${ROOT}/ta_compat/src/cob.cpp
    ${ROOT}/ta_compat/src/crt_strtod.cpp
    ${ROOT}/ta_compat/src/featuredefs.cpp
    ${ROOT}/ta_compat/src/gaf.cpp
    ${ROOT}/ta_compat/src/hpi.cpp
    ${ROOT}/ta_compat/src/inflate.cpp
    ${ROOT}/ta_compat/src/model3do.cpp
    ${ROOT}/ta_compat/src/tdf.cpp
    ${ROOT}/ta_compat/src/tnt.cpp
    ${ROOT}/ta_compat/src/unitdefs.cpp
    ${ROOT}/ta_compat/src/weapondefs.cpp
    ${ROOT}/engine/sim/src/heightmap.cpp
    ${ROOT}/engine/sim/src/pose.cpp
    ${ROOT}/engine/sim/src/rng.cpp
    ${ROOT}/engine/sim/src/script_vm.cpp
    ${ROOT}/engine/sim/src/unit.cpp
    ${ROOT}/engine/sim/src/unit_tick.cpp
    ${ROOT}/engine/sim/src/x87_math.cpp
    ${ROOT}/engine/sim/src/x87_trig.cpp
)
target_include_directories(unitloop_harness PRIVATE ${ROOT}/ta_compat/include ${ROOT}/engine/sim/include)
target_compile_options(unitloop_harness PRIVATE /W4 /permissive- /fp:precise)

# Damage, healing and death with the unit pass vs sim/combat.h.
add_executable(combat_harness
    combat_harness.cpp
    ${ROOT}/ta_compat/src/cob.cpp
    ${ROOT}/ta_compat/src/crt_strtod.cpp
    ${ROOT}/ta_compat/src/featuredefs.cpp
    ${ROOT}/ta_compat/src/gaf.cpp
    ${ROOT}/ta_compat/src/hpi.cpp
    ${ROOT}/ta_compat/src/inflate.cpp
    ${ROOT}/ta_compat/src/model3do.cpp
    ${ROOT}/ta_compat/src/tdf.cpp
    ${ROOT}/ta_compat/src/tnt.cpp
    ${ROOT}/ta_compat/src/unitdefs.cpp
    ${ROOT}/ta_compat/src/weapondefs.cpp
    ${ROOT}/engine/sim/src/combat.cpp
    ${ROOT}/engine/sim/src/heightmap.cpp
    ${ROOT}/engine/sim/src/pose.cpp
    ${ROOT}/engine/sim/src/rng.cpp
    ${ROOT}/engine/sim/src/script_vm.cpp
    ${ROOT}/engine/sim/src/trig.cpp
    ${ROOT}/engine/sim/src/unit.cpp
    ${ROOT}/engine/sim/src/unit_tick.cpp
    ${ROOT}/engine/sim/src/x87_math.cpp
    ${ROOT}/engine/sim/src/x87_trig.cpp
)
target_include_directories(combat_harness PRIVATE ${ROOT}/ta_compat/include ${ROOT}/engine/sim/include)
target_compile_options(combat_harness PRIVATE /W4 /permissive- /fp:precise)

# Unit order lists and ground settle (with the unit pass, damage and death) vs sim/orders.h and sim/ground_settle.h.
add_executable(orders_harness
    orders_harness.cpp
    ${ROOT}/ta_compat/src/cob.cpp
    ${ROOT}/ta_compat/src/crt_strtod.cpp
    ${ROOT}/ta_compat/src/featuredefs.cpp
    ${ROOT}/ta_compat/src/gaf.cpp
    ${ROOT}/ta_compat/src/hpi.cpp
    ${ROOT}/ta_compat/src/inflate.cpp
    ${ROOT}/ta_compat/src/model3do.cpp
    ${ROOT}/ta_compat/src/tdf.cpp
    ${ROOT}/ta_compat/src/tnt.cpp
    ${ROOT}/ta_compat/src/unitdefs.cpp
    ${ROOT}/ta_compat/src/weapondefs.cpp
    ${ROOT}/engine/sim/src/combat.cpp
    ${ROOT}/engine/sim/src/ground_settle.cpp
    ${ROOT}/engine/sim/src/heightmap.cpp
    ${ROOT}/engine/sim/src/missions.cpp
    ${ROOT}/engine/sim/src/orders.cpp
    ${ROOT}/engine/sim/src/pose.cpp
    ${ROOT}/engine/sim/src/rng.cpp
    ${ROOT}/engine/sim/src/script_vm.cpp
    ${ROOT}/engine/sim/src/trig.cpp
    ${ROOT}/engine/sim/src/unit.cpp
    ${ROOT}/engine/sim/src/unit_tick.cpp
    ${ROOT}/engine/sim/src/weapons.cpp
    ${ROOT}/engine/sim/src/x87_math.cpp
    ${ROOT}/engine/sim/src/x87_trig.cpp
)
target_include_directories(orders_harness PRIVATE ${ROOT}/ta_compat/include ${ROOT}/engine/sim/include)
target_compile_options(orders_harness PRIVATE /W4 /permissive- /fp:precise)

# Weapon ballistics (C runtime acos + pitch solver) vs sim/weapons.h.
add_executable(ballistic_check ballistic_check.cpp
    ${ROOT}/engine/sim/src/weapons.cpp
    ${ROOT}/engine/sim/src/heightmap.cpp
    ${ROOT}/engine/sim/src/pose.cpp
    ${ROOT}/engine/sim/src/script_vm.cpp
    ${ROOT}/engine/sim/src/unit.cpp
    ${ROOT}/engine/sim/src/x87_math.cpp
    ${ROOT}/engine/sim/src/x87_trig.cpp
)
target_include_directories(ballistic_check PRIVATE ${ROOT}/engine/sim/include)
target_compile_options(ballistic_check PRIVATE /W4 /permissive- /fp:precise)

# Weapon slot pass and aiming helpers with the unit pass vs sim/weapons.h.
add_executable(weapons_harness
    weapons_harness.cpp
    ${ROOT}/ta_compat/src/cob.cpp
    ${ROOT}/ta_compat/src/crt_strtod.cpp
    ${ROOT}/ta_compat/src/featuredefs.cpp
    ${ROOT}/ta_compat/src/gaf.cpp
    ${ROOT}/ta_compat/src/hpi.cpp
    ${ROOT}/ta_compat/src/inflate.cpp
    ${ROOT}/ta_compat/src/model3do.cpp
    ${ROOT}/ta_compat/src/tdf.cpp
    ${ROOT}/ta_compat/src/tnt.cpp
    ${ROOT}/ta_compat/src/unitdefs.cpp
    ${ROOT}/ta_compat/src/weapondefs.cpp
    ${ROOT}/engine/sim/src/combat.cpp
    ${ROOT}/engine/sim/src/heightmap.cpp
    ${ROOT}/engine/sim/src/pose.cpp
    ${ROOT}/engine/sim/src/rng.cpp
    ${ROOT}/engine/sim/src/script_vm.cpp
    ${ROOT}/engine/sim/src/trig.cpp
    ${ROOT}/engine/sim/src/unit.cpp
    ${ROOT}/engine/sim/src/unit_tick.cpp
    ${ROOT}/engine/sim/src/weapons.cpp
    ${ROOT}/engine/sim/src/x87_math.cpp
    ${ROOT}/engine/sim/src/x87_trig.cpp
)
target_include_directories(weapons_harness PRIVATE ${ROOT}/ta_compat/include ${ROOT}/engine/sim/include)
target_compile_options(weapons_harness PRIVATE /W4 /permissive- /fp:precise)

# Projectile system (table, launches, flight, impacts, area damage) with the unit pass vs sim/projectile.h.
add_executable(projectile_harness
    projectile_harness.cpp
    ${ROOT}/ta_compat/src/cob.cpp
    ${ROOT}/ta_compat/src/crt_strtod.cpp
    ${ROOT}/ta_compat/src/featuredefs.cpp
    ${ROOT}/ta_compat/src/gaf.cpp
    ${ROOT}/ta_compat/src/hpi.cpp
    ${ROOT}/ta_compat/src/inflate.cpp
    ${ROOT}/ta_compat/src/model3do.cpp
    ${ROOT}/ta_compat/src/tdf.cpp
    ${ROOT}/ta_compat/src/tnt.cpp
    ${ROOT}/ta_compat/src/unitdefs.cpp
    ${ROOT}/ta_compat/src/weapondefs.cpp
    ${ROOT}/engine/sim/src/combat.cpp
    ${ROOT}/engine/sim/src/heightmap.cpp
    ${ROOT}/engine/sim/src/pose.cpp
    ${ROOT}/engine/sim/src/fire.cpp
    ${ROOT}/engine/sim/src/weapons.cpp
    ${ROOT}/engine/sim/src/projectile.cpp
    ${ROOT}/engine/sim/src/rng.cpp
    ${ROOT}/engine/sim/src/script_vm.cpp
    ${ROOT}/engine/sim/src/trig.cpp
    ${ROOT}/engine/sim/src/unit.cpp
    ${ROOT}/engine/sim/src/unit_tick.cpp
    ${ROOT}/engine/sim/src/x87_math.cpp
    ${ROOT}/engine/sim/src/x87_trig.cpp
)
target_include_directories(projectile_harness PRIVATE ${ROOT}/ta_compat/include ${ROOT}/engine/sim/include)
target_compile_options(projectile_harness PRIVATE /W4 /permissive- /fp:precise)

# Polygon scan conversion: original flat depth polygon and textured quad vs render::fill_polygon / textured_quad.
add_executable(raster_harness raster_harness.cpp ${ROOT}/render/src/raster.cpp ${ROOT}/render/src/image.cpp)
target_include_directories(raster_harness PRIVATE ${ROOT}/render/include ${ROOT}/engine/sim/include)
target_compile_options(raster_harness PRIVATE /W4 /permissive- /fp:precise)

# Unit creation and footprints through sim::Battle vs the original creation routine.
add_executable(create_harness
    create_harness.cpp
    ${ROOT}/ta_compat/src/battle_types.cpp
    ${ROOT}/ta_compat/src/cob.cpp
    ${ROOT}/ta_compat/src/crt_strtod.cpp
    ${ROOT}/ta_compat/src/featuredefs.cpp
    ${ROOT}/ta_compat/src/gaf.cpp
    ${ROOT}/ta_compat/src/hpi.cpp
    ${ROOT}/ta_compat/src/inflate.cpp
    ${ROOT}/ta_compat/src/model3do.cpp
    ${ROOT}/ta_compat/src/tdf.cpp
    ${ROOT}/ta_compat/src/tnt.cpp
    ${ROOT}/ta_compat/src/unitdefs.cpp
    ${ROOT}/ta_compat/src/weapondefs.cpp
    ${ROOT}/engine/sim/src/battle.cpp
    ${ROOT}/engine/sim/src/route_search.cpp
    ${ROOT}/engine/sim/src/targeting.cpp
    ${ROOT}/engine/sim/src/missions.cpp
    ${ROOT}/engine/sim/src/motion.cpp
    ${ROOT}/engine/sim/src/combat.cpp
    ${ROOT}/engine/sim/src/fire.cpp
    ${ROOT}/engine/sim/src/footprint.cpp
    ${ROOT}/engine/sim/src/ground_settle.cpp
    ${ROOT}/engine/sim/src/heightmap.cpp
    ${ROOT}/engine/sim/src/orders.cpp
    ${ROOT}/engine/sim/src/pose.cpp
    ${ROOT}/engine/sim/src/projectile.cpp
    ${ROOT}/engine/sim/src/rng.cpp
    ${ROOT}/engine/sim/src/script_vm.cpp
    ${ROOT}/engine/sim/src/trig.cpp
    ${ROOT}/engine/sim/src/unit.cpp
    ${ROOT}/engine/sim/src/unit_create.cpp
    ${ROOT}/engine/sim/src/unit_tick.cpp
    ${ROOT}/engine/sim/src/weapons.cpp
    ${ROOT}/engine/sim/src/x87_math.cpp
    ${ROOT}/engine/sim/src/x87_trig.cpp
)
target_include_directories(create_harness PRIVATE ${ROOT}/ta_compat/include ${ROOT}/engine/sim/include)
target_compile_options(create_harness PRIVATE /W4 /permissive- /fp:precise)

# Unit motion (motion step, goals, order goals) with the unit pass and ground settle vs sim/motion.h.
add_executable(motion_harness
    motion_harness.cpp
    ${ROOT}/ta_compat/src/cob.cpp
    ${ROOT}/ta_compat/src/crt_strtod.cpp
    ${ROOT}/ta_compat/src/featuredefs.cpp
    ${ROOT}/ta_compat/src/gaf.cpp
    ${ROOT}/ta_compat/src/hpi.cpp
    ${ROOT}/ta_compat/src/inflate.cpp
    ${ROOT}/ta_compat/src/model3do.cpp
    ${ROOT}/ta_compat/src/tdf.cpp
    ${ROOT}/ta_compat/src/tnt.cpp
    ${ROOT}/ta_compat/src/unitdefs.cpp
    ${ROOT}/ta_compat/src/weapondefs.cpp
    ${ROOT}/engine/sim/src/ground_settle.cpp
    ${ROOT}/engine/sim/src/heightmap.cpp
    ${ROOT}/engine/sim/src/motion.cpp
    ${ROOT}/engine/sim/src/pose.cpp
    ${ROOT}/engine/sim/src/rng.cpp
    ${ROOT}/engine/sim/src/script_vm.cpp
    ${ROOT}/engine/sim/src/trig.cpp
    ${ROOT}/engine/sim/src/unit.cpp
    ${ROOT}/engine/sim/src/unit_tick.cpp
    ${ROOT}/engine/sim/src/x87_math.cpp
    ${ROOT}/engine/sim/src/x87_trig.cpp
)
target_include_directories(motion_harness PRIVATE ${ROOT}/ta_compat/include ${ROOT}/engine/sim/include)
target_compile_options(motion_harness PRIVATE /W4 /permissive- /fp:precise)

# Target acquisition (enemy lists, best target, attack orders, mission resolution, retargeting,
# weapon-target pass) vs sim/targeting.h.
add_executable(targeting_harness
    targeting_harness.cpp
    ${ROOT}/ta_compat/src/cob.cpp
    ${ROOT}/ta_compat/src/crt_strtod.cpp
    ${ROOT}/ta_compat/src/featuredefs.cpp
    ${ROOT}/ta_compat/src/gaf.cpp
    ${ROOT}/ta_compat/src/hpi.cpp
    ${ROOT}/ta_compat/src/inflate.cpp
    ${ROOT}/ta_compat/src/model3do.cpp
    ${ROOT}/ta_compat/src/tdf.cpp
    ${ROOT}/ta_compat/src/tnt.cpp
    ${ROOT}/ta_compat/src/unitdefs.cpp
    ${ROOT}/ta_compat/src/weapondefs.cpp
    ${ROOT}/engine/sim/src/combat.cpp
    ${ROOT}/engine/sim/src/heightmap.cpp
    ${ROOT}/engine/sim/src/missions.cpp
    ${ROOT}/engine/sim/src/orders.cpp
    ${ROOT}/engine/sim/src/pose.cpp
    ${ROOT}/engine/sim/src/rng.cpp
    ${ROOT}/engine/sim/src/script_vm.cpp
    ${ROOT}/engine/sim/src/targeting.cpp
    ${ROOT}/engine/sim/src/trig.cpp
    ${ROOT}/engine/sim/src/unit.cpp
    ${ROOT}/engine/sim/src/unit_tick.cpp
    ${ROOT}/engine/sim/src/weapons.cpp
    ${ROOT}/engine/sim/src/x87_math.cpp
    ${ROOT}/engine/sim/src/x87_trig.cpp
)
target_include_directories(targeting_harness PRIVATE ${ROOT}/ta_compat/include ${ROOT}/engine/sim/include)
target_compile_options(targeting_harness PRIVATE /W4 /permissive- /fp:precise)

# Movement classes' passability, placement checks and the route search vs sim/route_search.h.
add_executable(route_harness
    route_harness.cpp
    ${ROOT}/ta_compat/src/cob.cpp
    ${ROOT}/ta_compat/src/crt_strtod.cpp
    ${ROOT}/ta_compat/src/featuredefs.cpp
    ${ROOT}/ta_compat/src/gaf.cpp
    ${ROOT}/ta_compat/src/hpi.cpp
    ${ROOT}/ta_compat/src/inflate.cpp
    ${ROOT}/ta_compat/src/model3do.cpp
    ${ROOT}/ta_compat/src/tdf.cpp
    ${ROOT}/ta_compat/src/unitdefs.cpp
    ${ROOT}/ta_compat/src/weapondefs.cpp
    ${ROOT}/engine/sim/src/heightmap.cpp
    ${ROOT}/engine/sim/src/motion.cpp
    ${ROOT}/engine/sim/src/pose.cpp
    ${ROOT}/engine/sim/src/rng.cpp
    ${ROOT}/engine/sim/src/route_search.cpp
    ${ROOT}/engine/sim/src/script_vm.cpp
    ${ROOT}/engine/sim/src/trig.cpp
    ${ROOT}/engine/sim/src/unit.cpp
    ${ROOT}/engine/sim/src/x87_math.cpp
    ${ROOT}/engine/sim/src/x87_trig.cpp
)
target_include_directories(route_harness PRIVATE ${ROOT}/ta_compat/include ${ROOT}/engine/sim/include)
target_compile_options(route_harness PRIVATE /W4 /permissive- /fp:precise)
