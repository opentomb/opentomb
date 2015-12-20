#include "engine.h"

#include <cctype>
#include <cstdio>

#include <btBulletCollisionCommon.h>
#include <btBulletDynamicsCommon.h>
#include <BulletCollision/CollisionDispatch/btGhostObject.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_video.h>
#include <SDL2/SDL_events.h>
#include <SDL2/SDL_haptic.h>

#include <glm/gtx/string_cast.hpp>

#if !defined(__MACOSX__)
#include <SDL2/SDL_image.h>
#endif

#if defined(__MACOSX__)
#include "mac/FindConfigFile.h"
#endif

#include "LuaState.h"

#include "common.h"
#include "controls.h"
#include "engine/game.h"
#include "engine/system.h"
#include "gameflow.h"
#include "gui/console.h"
#include "gui/fader.h"
#include "gui/gui.h"
#include "gui/itemnotifier.h"
#include "inventory.h"
#include "loader/level.h"
#include "render/render.h"
#include "script/script.h"
#include "strings.h"
#include "world/camera.h"
#include "world/character.h"
#include "world/core/polygon.h"
#include "world/entity.h"
#include "world/resource.h"
#include "world/room.h"
#include "world/skeletalmodel.h"
#include "world/staticmesh.h"
#include "world/world.h"

#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include <boost/filesystem.hpp>

using gui::Console;

namespace render
{
RenderDebugDrawer                    debugDrawer;
} // namespace render

namespace engine
{

SDL_Window             *sdl_window = nullptr;
SDL_Joystick           *sdl_joystick = nullptr;
SDL_GameController     *sdl_controller = nullptr;
SDL_Haptic             *sdl_haptic = nullptr;
SDL_GLContext           sdl_gl_context = nullptr;

EngineControlState control_states{};
ControlSettings    control_mapper{};

util::Duration engine_frame_time = util::Duration(0);

world::Camera             engine_camera;
world::World              engine_world;

namespace
{
std::vector<glm::float_t> frame_vertex_buffer;
size_t frame_vertex_buffer_size_left = 0;
}

btDefaultCollisionConfiguration     *bt_engine_collisionConfiguration = nullptr;
btCollisionDispatcher               *bt_engine_dispatcher = nullptr;
btGhostPairCallback                 *bt_engine_ghostPairCallback = nullptr;
btBroadphaseInterface               *bt_engine_overlappingPairCache = nullptr;
btSequentialImpulseConstraintSolver *bt_engine_solver = nullptr;
btDiscreteDynamicsWorld             *bt_engine_dynamicsWorld = nullptr;
btOverlapFilterCallback             *bt_engine_filterCallback = nullptr;


// Debug globals.

glm::vec3 light_position = { 255.0, 255.0, 8.0 };
GLfloat cast_ray[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };

world::Object* last_object = nullptr;

void initGL()
{
    glewExperimental = GL_TRUE;
    glewInit();

    // GLEW sometimes causes an OpenGL error for no apparent reason. Retrieve and
    // discard it so it doesn't clog up later logging.

    glGetError();

    glClearColor(0.0, 0.0, 0.0, 1.0);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);

    if(::render::renderer.settings().antialias)
    {
        glEnable(GL_MULTISAMPLE);
    }
    else
    {
        glDisable(GL_MULTISAMPLE);
    }
}

void initSDLControls()
{
    Uint32 init_flags = SDL_INIT_VIDEO | SDL_INIT_EVENTS; // These flags are used in any case.

    if(control_mapper.use_joy)
    {
        init_flags |= SDL_INIT_GAMECONTROLLER;  // Update init flags for joystick.

        if(control_mapper.joy_rumble)
        {
            init_flags |= SDL_INIT_HAPTIC;      // Update init flags for force feedback.
        }

        SDL_Init(init_flags);

        int NumJoysticks = SDL_NumJoysticks();

        if((NumJoysticks < 1) || ((NumJoysticks - 1) < control_mapper.joy_number))
        {
            BOOST_LOG_TRIVIAL(error) << "There is no joystick #" << control_mapper.joy_number << " present";
            return;
        }

        if(SDL_IsGameController(control_mapper.joy_number))                     // If joystick has mapping (e.g. X360 controller)
        {
            SDL_GameControllerEventState(SDL_ENABLE);                           // Use GameController API
            sdl_controller = SDL_GameControllerOpen(control_mapper.joy_number);

            if(!sdl_controller)
            {
                BOOST_LOG_TRIVIAL(error) << "Can't open game controller #d" << control_mapper.joy_number;
                SDL_GameControllerEventState(SDL_DISABLE);                      // If controller init failed, close state.
                control_mapper.use_joy = false;
            }
            else if(control_mapper.joy_rumble)                                  // Create force feedback interface.
            {
                sdl_haptic = SDL_HapticOpenFromJoystick(SDL_GameControllerGetJoystick(sdl_controller));
                if(!sdl_haptic)
                {
                    BOOST_LOG_TRIVIAL(error) << "Can't initialize haptic from game controller #" << control_mapper.joy_number;
                }
            }
        }
        else
        {
            SDL_JoystickEventState(SDL_ENABLE);                                 // If joystick isn't mapped, use generic API.
            sdl_joystick = SDL_JoystickOpen(control_mapper.joy_number);

            if(!sdl_joystick)
            {
                BOOST_LOG_TRIVIAL(error) << "Can't open joystick #" << control_mapper.joy_number;
                SDL_JoystickEventState(SDL_DISABLE);                            // If joystick init failed, close state.
                control_mapper.use_joy = false;
            }
            else if(control_mapper.joy_rumble)                                  // Create force feedback interface.
            {
                sdl_haptic = SDL_HapticOpenFromJoystick(sdl_joystick);
                if(!sdl_haptic)
                {
                    BOOST_LOG_TRIVIAL(error) << "Can't initialize haptic from joystick #" << control_mapper.joy_number;
                }
            }
        }

        if(sdl_haptic)                                                          // To check if force feedback is working or not.
        {
            SDL_HapticRumbleInit(sdl_haptic);
            SDL_HapticRumblePlay(sdl_haptic, 1.0, 300);
        }
    }
    else
    {
        SDL_Init(init_flags);
    }
}

void initSDLVideo()
{
    Uint32 video_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_MOUSE_FOCUS | SDL_WINDOW_INPUT_FOCUS;

    if(screen_info.FS_flag)
    {
        video_flags |= SDL_WINDOW_FULLSCREEN;
    }
    else
    {
        video_flags |= (SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    }

    ///@TODO: is it really needed for correct work?

    if(SDL_GL_LoadLibrary(nullptr) < 0)
    {
        BOOST_THROW_EXCEPTION(std::runtime_error("Could not init OpenGL driver"));
    }

    if(render::renderer.settings().use_gl3)
    {
        /* Request opengl 3.2 context. */
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    }

    // Create temporary SDL window and GL context for checking capabilities.

    sdl_window = SDL_CreateWindow(nullptr, screen_info.x, screen_info.y, screen_info.w, screen_info.h, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    sdl_gl_context = SDL_GL_CreateContext(sdl_window);

    if(!sdl_gl_context)
        BOOST_THROW_EXCEPTION(std::runtime_error("Can't create OpenGL context - shutting down. Try to disable use_gl3 option in config."));

    BOOST_ASSERT(sdl_gl_context);
    SDL_GL_MakeCurrent(sdl_window, sdl_gl_context);

    // Check for correct number of antialias samples.

    if(render::renderer.settings().antialias)
    {
        GLint maxSamples = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
        maxSamples = (maxSamples > 16) ? (16) : (maxSamples);   // Fix for faulty GL max. sample number.

        if(render::renderer.settings().antialias_samples > maxSamples)
        {
            if(maxSamples == 0)
            {
                render::renderer.settings().antialias = 0;
                render::renderer.settings().antialias_samples = 0;
                BOOST_LOG_TRIVIAL(error) << "InitSDLVideo: can't use antialiasing";
            }
            else
            {
                render::renderer.settings().antialias_samples = maxSamples;   // Limit to max.
                BOOST_LOG_TRIVIAL(error) << "InitSDLVideo: wrong AA sample number, using " << maxSamples;
            }
        }

        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, render::renderer.settings().antialias);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, render::renderer.settings().antialias_samples);
    }
    else
    {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
    }

    // Remove temporary GL context and SDL window.

    SDL_GL_DeleteContext(sdl_gl_context);
    SDL_DestroyWindow(sdl_window);

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, render::renderer.settings().z_depth);

    sdl_window = SDL_CreateWindow("OpenTomb", screen_info.x, screen_info.y, screen_info.w, screen_info.h, video_flags);
    sdl_gl_context = SDL_GL_CreateContext(sdl_window);
    SDL_GL_MakeCurrent(sdl_window, sdl_gl_context);

    if(SDL_GL_SetSwapInterval(screen_info.vsync))
        BOOST_LOG_TRIVIAL(error) << "Cannot set VSYNC: " << SDL_GetError();

    Console::instance().addLine(reinterpret_cast<const char*>(glGetString(GL_VENDOR)), gui::FontStyle::ConsoleInfo);
    Console::instance().addLine(reinterpret_cast<const char*>(glGetString(GL_RENDERER)), gui::FontStyle::ConsoleInfo);
    std::string version = "OpenGL version ";
    version += reinterpret_cast<const char*>(glGetString(GL_VERSION));
    Console::instance().addLine(version, gui::FontStyle::ConsoleInfo);
    Console::instance().addLine(reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)), gui::FontStyle::ConsoleInfo);
}

#if !defined(__MACOSX__)
void initSDLImage()
{
    int flags = IMG_INIT_JPG | IMG_INIT_PNG;
    int init = IMG_Init(flags);

    if((init & flags) != flags)
    {
        BOOST_LOG_TRIVIAL(error) << "SDL_Image: failed to initialize JPG and/or PNG support.";
    }
}
#endif

void start()
{
#if defined(__MACOSX__)
    FindConfigFile();
#endif

    // Set defaults parameters and load config file.
    initConfig("config.lua");

    // Primary initialization.
    initPre();

    // Init generic SDL interfaces.
    initSDLControls();
    initSDLVideo();

#if !defined(__MACOSX__)
    initSDLImage();
#endif

    // Additional OpenGL initialization.
    initGL();
    render::renderer.doShaders();

    // Secondary (deferred) initialization.
    initPost();

    // Initial window resize.
    resize(screen_info.w, screen_info.h, screen_info.w, screen_info.h);

    // OpenAL initialization.
    engine_world.audioEngine.initDevice();

    Console::instance().notify(SYSNOTE_ENGINE_INITED);

    // Clearing up memory for initial level loading.
    engine_world.prepare();

    SDL_SetRelativeMouseMode(SDL_TRUE);

    // Make splash screen.
    gui::fadeAssignPic(gui::FaderType::LoadScreen, "resource/graphics/legal.png");
    gui::fadeStart(gui::FaderType::LoadScreen, gui::FaderDir::Out);

    engine_lua.doFile("autoexec.lua");
}

void display()
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);//| GL_ACCUM_BUFFER_BIT);

    engine_camera.apply();
    // GL_VERTEX_ARRAY | GL_COLOR_ARRAY
    if(screen_info.show_debuginfo)
    {
        showDebugInfo();
    }

    glFrontFace(GL_CW);

    render::renderer.genWorldList();
    render::renderer.drawList();

    //glDisable(GL_CULL_FACE);
    //Render_DrawAxis(10000.0);
    /*if(engine_world.character)
    {
        glPushMatrix();
        glTranslatef(engine_world.character->transform[12], engine_world.character->transform[13], engine_world.character->transform[14]);
        Render_DrawAxis(1000.0);
        glPopMatrix();
    }*/

    gui::switchGLMode(true);
    {
        gui::drawNotifier();
        if(engine_world.character && main_inventory_manager)
        {
            gui::drawInventory();
        }
    }

    gui::render();
    gui::switchGLMode(false);

    render::renderer.drawListDebugLines();

    SDL_GL_SwapWindow(sdl_window);
}

void resize(int nominalW, int nominalH, int pixelsW, int pixelsH)
{
    screen_info.w = nominalW;
    screen_info.h = nominalH;

    screen_info.w_unit = static_cast<float>(nominalW) / gui::ScreenMeteringResolution;
    screen_info.h_unit = static_cast<float>(nominalH) / gui::ScreenMeteringResolution;
    screen_info.scale_factor = (screen_info.w < screen_info.h) ? (screen_info.h_unit) : (screen_info.w_unit);

    gui::resize();

    engine_camera.setFovAspect(screen_info.fov, static_cast<glm::float_t>(nominalW) / static_cast<glm::float_t>(nominalH));
    engine_camera.apply();

    glViewport(0, 0, pixelsW, pixelsH);
}

extern gui::TextLine system_fps;

namespace
{
    int fpsCycles = 0;
    util::Duration fpsTime = util::Duration(0);

    void fpsCycle(util::Duration time)
    {
        if(fpsCycles < 20)
        {
            fpsCycles++;
            fpsTime += time;
        }
        else
        {
            screen_info.fps = (20.0f / util::toSeconds(fpsTime));
            char tmp[16];
            snprintf(tmp, 16, "%.1f", screen_info.fps);
            system_fps.text = tmp;
            fpsCycles = 0;
            fpsTime = util::Duration(0);
        }
    }
}

void frame(util::Duration time)
{
    engine_frame_time = time;
    fpsCycle(time);

    Game_Frame(time);
    Gameflow_Manager.execute();
}

void showDebugInfo()
{
    GLfloat color_array[] = { 1.0, 0.0, 0.0, 1.0, 0.0, 0.0 };

    light_position = engine_camera.getPosition();

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glLineWidth(2.0);
    glVertexPointer(3, GL_FLOAT, 0, cast_ray);
    glColorPointer(3, GL_FLOAT, 0, color_array);
    glDrawArrays(GL_LINES, 0, 2);

    if(std::shared_ptr<world::Character> ent = engine_world.character)
    {
        /*height_info_p fc = &ent->character->height_info
        txt = Gui_OutTextXY(20.0 / screen_info.w, 80.0 / screen_info.w, "Z_min = %d, Z_max = %d, W = %d", (int)fc->floor_point[2], (int)fc->ceiling_point[2], (int)fc->water_level);
        */
        gui::drawText(30.0, 30.0, "prevState = %03d, nextState = %03d, speed = %f",
                      ent->m_skeleton.getPreviousState(),
                      ent->m_skeleton.getCurrentState(),
                      ent->m_currentSpeed
                      );
        gui::drawText(30.0, 50.0, "prevAnim = %3d, prevFrame = %3d, currAnim = %3d, currFrame = %3d",
                ent->m_skeleton.getPreviousAnimation(),
                ent->m_skeleton.getPreviousFrame(),
                ent->m_skeleton.getCurrentAnimation(),
                ent->m_skeleton.getCurrentFrame()
                );
        //Gui_OutTextXY(30.0, 30.0, "curr_anim = %03d, next_anim = %03d, curr_frame = %03d, next_frame = %03d", ent->bf.animations.current_animation, ent->bf.animations.next_animation, ent->bf.animations.current_frame, ent->bf.animations.next_frame);
        gui::drawText(20, 8, "pos = %s, yaw = %f", glm::to_string(ent->m_transform[3]).c_str(), ent->m_angles[0]);
    }

    if(last_object != nullptr)
    {
        if(world::Entity* e = dynamic_cast<world::Entity*>(last_object))
        {
            gui::drawText(30.0, 60.0, "cont_entity: id = %d, model = %d", e->getId(), e->m_skeleton.getModel()->id);
        }
        else if(world::StaticMesh* sm = dynamic_cast<world::StaticMesh*>(last_object))
        {
            gui::drawText(30.0, 60.0, "cont_static: id = %d", sm->getId());
        }
        else if(world::Room* r = dynamic_cast<world::Room*>(last_object))
        {
            gui::drawText(30.0, 60.0, "cont_room: id = %d", r->getId());
        }
    }

    if(engine_camera.getCurrentRoom() != nullptr)
    {
        world::RoomSector* rs = engine_camera.getCurrentRoom()->getSectorRaw(engine_camera.getPosition());
        if(rs != nullptr)
        {
            gui::drawText(30.0, 90.0, "room = (id = %d, sx = %d, sy = %d)", engine_camera.getCurrentRoom()->getId(), rs->index_x, rs->index_y);
            gui::drawText(30.0, 120.0, "room_below = %d, room_above = %d", (rs->sector_below != nullptr) ? (rs->sector_below->owner_room->getId()) : (-1), (rs->sector_above != nullptr) ? (rs->sector_above->owner_room->getId()) : (-1));
        }
    }
    gui::drawText(30.0, 150.0, "cam_pos = %s", glm::to_string(engine_camera.getPosition()).c_str());
}

/**
 * overlapping room collision filter
 */
void roomNearCallback(btBroadphasePair& collisionPair, btCollisionDispatcher& dispatcher, const btDispatcherInfo& dispatchInfo)
{
    world::Object* c0 = static_cast<world::Object*>(static_cast<btCollisionObject*>(collisionPair.m_pProxy0->m_clientObject)->getUserPointer());
    world::Room* r0 = c0 ? c0->getRoom() : nullptr;
    world::Object* c1 = static_cast<world::Object*>(static_cast<btCollisionObject*>(collisionPair.m_pProxy1->m_clientObject)->getUserPointer());
    world::Room* r1 = c1 ? c1->getRoom() : nullptr;

    if(c1 && c1 == c0)
    {
        if(static_cast<btCollisionObject*>(collisionPair.m_pProxy0->m_clientObject)->isStaticOrKinematicObject() ||
           static_cast<btCollisionObject*>(collisionPair.m_pProxy1->m_clientObject)->isStaticOrKinematicObject())
        {
            return;                                                             // No self interaction
        }
        dispatcher.defaultNearCallback(collisionPair, dispatcher, dispatchInfo);
        return;
    }

    if(!r0 && !r1)
    {
        dispatcher.defaultNearCallback(collisionPair, dispatcher, dispatchInfo);// Both are out of rooms
        return;
    }

    if(r0 && r1 && r0->isInNearRoomsList(*r1))
    {
        dispatcher.defaultNearCallback(collisionPair, dispatcher, dispatchInfo);
    }
}

void storeEntityLerpTransforms()
{
    if(engine_world.character)
    {
        if(!(engine_world.character->m_typeFlags & ENTITY_TYPE_DYNAMIC))
        {
#if 0
            if(engine_world.character->m_lerp_skip)
            {
                engine_world.character->m_skeleton.setPreviousAnimation(engine_world.character->m_skeleton.getCurrentAnimation());
                engine_world.character->m_skeleton.setPreviousFrame(engine_world.character->m_skeleton.getCurrentFrame());
                engine_world.character->m_lerp_skip = false;
            }
#endif

            // set bones to next interval step, this keeps the ponytail (bullet's dynamic interpolation) in sync with actor interpolation:
            engine_world.character->m_skeleton.updatePose();
            engine_world.character->updateRigidBody(false);
            engine_world.character->ghostUpdate();
        }
    }

    for(auto entityPair : engine_world.entity_tree)
    {
        std::shared_ptr<world::Entity> entity = entityPair.second;
        if(!entity->m_enabled)
            continue;

        if((entity->m_typeFlags & ENTITY_TYPE_DYNAMIC) != 0)
            continue;

#if 0
        if(entity->m_lerp_skip)
        {
            entity->m_skeleton.setPreviousAnimation(entity->m_skeleton.getCurrentAnimation());
            entity->m_skeleton.setPreviousFrame(entity->m_skeleton.getCurrentFrame());
            entity->m_lerp_skip = false;
        }
#endif

        entity->m_skeleton.updatePose();
        entity->updateRigidBody(false);
        entity->ghostUpdate();
    }
}


/**
 * Pre-physics step callback
 */
void internalPreTickCallback(btDynamicsWorld* /*world*/, float timeStep)
{
    util::Duration engine_frame_time_backup = engine_frame_time;
    engine_frame_time = util::fromSeconds(timeStep);

    engine_lua.doTasks(engine_frame_time_backup);
    Game_UpdateAI();
    engine::engine_world.audioEngine.updateAudio();

    if(engine_world.character)
    {
        engine_world.character->frame(util::fromSeconds(timeStep));
    }
    for(const auto& entPair : engine_world.entity_tree)
    {
        entPair.second->frame(util::fromSeconds(timeStep));
    }

    storeEntityLerpTransforms();
    engine_frame_time = engine_frame_time_backup;
    return;
}

/**
 * Post-physics step callback
 */
void internalTickCallback(btDynamicsWorld *world, float /*timeStep*/)
{
    // Update all physics object's transform/room:
    for(int i = world->getNumCollisionObjects() - 1; i >= 0; i--)
    {
        BOOST_ASSERT(i >= 0 && i < bt_engine_dynamicsWorld->getCollisionObjectArray().size());
        btCollisionObject* obj = bt_engine_dynamicsWorld->getCollisionObjectArray()[i];
        btRigidBody* body = btRigidBody::upcast(obj);
        if(body && !body->isStaticObject() && body->getMotionState())
        {
            btTransform trans;
            body->getMotionState()->getWorldTransform(trans);
            world::Object* object = static_cast<world::Object*>(body->getUserPointer());
            if(dynamic_cast<world::BulletObject*>(object))
            {
                object->setRoom( Room_FindPosCogerrence(util::convert(trans.getOrigin()), object->getRoom()) );
            }
        }
    }
}

void initDefaultGlobals()
{
    Console::instance().initGlobals();
    Controls_InitGlobals();
    Game_InitGlobals();
    render::renderer.initGlobals();
    engine_world.audioEngine.resetSettings();
}

// First stage of initialization.

void initPre()
{
    /* Console must be initialized previously! some functions uses ConsoleInfo::instance().addLine before GL initialization!
     * Rendering activation may be done later. */

    gui::initFontManager();
    Console::instance().init();

    engine_lua["loadscript_pre"]();

    Gameflow_Manager.init();

    frame_vertex_buffer.resize(render::InitFrameVertexBufferSize);
    frame_vertex_buffer_size_left = frame_vertex_buffer.size();

    Console::instance().setCompletionItems(engine_lua.getGlobals());

    Com_Init();
    render::renderer.init();
    render::renderer.setCamera(&engine_camera);

    initBullet();
}

// Second stage of initialization.

void initPost()
{
    engine_lua["loadscript_post"]();

    Console::instance().initFonts();

    gui::init();
    Sys_Init();
}

// Bullet Physics initialization.

void initBullet()
{
    ///collision configuration contains default setup for memory, collision setup. Advanced users can create their own configuration.
    bt_engine_collisionConfiguration = new btDefaultCollisionConfiguration();

    ///use the default collision dispatcher. For parallel processing you can use a diffent dispatcher (see Extras/BulletMultiThreaded)
    bt_engine_dispatcher = new btCollisionDispatcher(bt_engine_collisionConfiguration);
    bt_engine_dispatcher->setNearCallback(roomNearCallback);

    ///btDbvtBroadphase is a good general purpose broadphase. You can also try out btAxis3Sweep.
    bt_engine_overlappingPairCache = new btDbvtBroadphase();
    bt_engine_ghostPairCallback = new btGhostPairCallback();
    bt_engine_overlappingPairCache->getOverlappingPairCache()->setInternalGhostPairCallback(bt_engine_ghostPairCallback);

    ///the default constraint solver. For parallel processing you can use a different solver (see Extras/BulletMultiThreaded)
    bt_engine_solver = new btSequentialImpulseConstraintSolver;

    bt_engine_dynamicsWorld = new btDiscreteDynamicsWorld(bt_engine_dispatcher, bt_engine_overlappingPairCache, bt_engine_solver, bt_engine_collisionConfiguration);
    bt_engine_dynamicsWorld->setInternalTickCallback(internalTickCallback);
    bt_engine_dynamicsWorld->setInternalTickCallback(internalPreTickCallback, nullptr, true);
    bt_engine_dynamicsWorld->setGravity(btVector3(0, 0, -4500.0));

    render::debugDrawer.setDebugMode(btIDebugDraw::DBG_DrawWireframe | btIDebugDraw::DBG_DrawConstraints);
    bt_engine_dynamicsWorld->setDebugDrawer(&render::debugDrawer);
    //bt_engine_dynamicsWorld->getPairCache()->setInternalGhostPairCallback(bt_engine_filterCallback);
}

void dumpRoom(world::Room* r)
{
    if(!r)
        return;

    BOOST_LOG_TRIVIAL(debug) << boost::format("ROOM = %d, (%d x %d), bottom = %g, top = %g, pos(%g, %g)")
                                % r->getId()
                                % r->m_sectors.shape()[0]
                                % r->m_sectors.shape()[1]
                                % r->m_boundingBox.min[2]
                                % r->m_boundingBox.max[2]
                                % r->m_modelMatrix[3][0]
                                % r->m_modelMatrix[3][1];
    BOOST_LOG_TRIVIAL(debug) << boost::format("flag = 0x%X, alt_room = %d, base_room = %d")
                                % r->m_flags
                                % (r->m_alternateRoom != nullptr ? r->m_alternateRoom->getId() : -1)
                                % (r->m_baseRoom != nullptr ? r->m_baseRoom->getId() : -1);
    for(const auto& column : r->m_sectors)
    {
        for(const world::RoomSector& rs : column)
        {
            BOOST_LOG_TRIVIAL(debug) << boost::format("(%d,%d) floor = %d, ceiling = %d, portal = %d")
                                        % rs.index_x
                                        % rs.index_y
                                        % rs.floor
                                        % rs.ceiling
                                        % rs.portal_to_room;
        }
    }

    for(auto sm : r->m_staticMeshes)
    {
        BOOST_LOG_TRIVIAL(debug) << "static_mesh = " << sm->getId();
    }
    for(world::Object* object : r->m_objects)
    {
        if(world::Entity* ent = dynamic_cast<world::Entity*>(object))
        {
            BOOST_LOG_TRIVIAL(debug) << "entity: id = " << ent->getId() << ", model = " << ent->m_skeleton.getModel()->id;
        }
    }
}

void destroy()
{
    render::renderer.empty();
    //ConsoleInfo::instance().destroy();
    Com_Destroy();
    Sys_Destroy();

    //delete dynamics world
    delete bt_engine_dynamicsWorld;

    //delete solver
    delete bt_engine_solver;

    //delete broadphase
    delete bt_engine_overlappingPairCache;

    //delete dispatcher
    delete bt_engine_dispatcher;

    delete bt_engine_collisionConfiguration;

    delete bt_engine_ghostPairCallback;

    gui::destroy();
}

void shutdown(int val)
{
    engine_lua.clearTasks();
    render::renderer.empty();
    engine_world.empty();
    destroy();

    /* no more renderings */
    SDL_GL_DeleteContext(sdl_gl_context);
    SDL_DestroyWindow(sdl_window);

    if(sdl_joystick)
    {
        SDL_JoystickClose(sdl_joystick);
    }

    if(sdl_controller)
    {
        SDL_GameControllerClose(sdl_controller);
    }

    if(sdl_haptic)
    {
        SDL_HapticClose(sdl_haptic);
    }

    engine_world.audioEngine.closeDevice();

    /* free temporary memory */
    frame_vertex_buffer.clear();
    frame_vertex_buffer_size_left = 0;

#if !defined(__MACOSX__)
    IMG_Quit();
#endif
    SDL_Quit();

    exit(val);
}

int getLevelFormat(const std::string& /*name*/)
{
    // PLACEHOLDER: Currently, only PC levels are supported.

    return LEVEL_FORMAT_PC;
}

std::string getLevelName(const std::string& path)
{
    if(path.empty())
    {
        return{};
    }

    size_t ext = path.find_last_of(".");
    BOOST_ASSERT(ext != std::string::npos);

    size_t start = path.find_last_of("\\/");
    if(start == std::string::npos)
        start = 0;
    else
        ++start;

    return path.substr(start, ext - start);
}

std::string getAutoexecName(loader::Game game_version, const std::string& postfix)
{
    std::string level_name = getLevelName(Gameflow_Manager.getLevelPath());

    std::string name = "scripts/autoexec/";

    if(game_version < loader::Game::TR2)
    {
        name += "tr1/";
    }
    else if(game_version < loader::Game::TR3)
    {
        name += "tr2/";
    }
    else if(game_version < loader::Game::TR4)
    {
        name += "tr3/";
    }
    else if(game_version < loader::Game::TR5)
    {
        name += "tr4/";
    }
    else
    {
        name += "tr5/";
    }

    for(char& c : level_name)
    {
        c = std::toupper(c);
    }

    name += level_name;
    name += postfix;
    name += ".lua";
    return name;
}

bool loadPCLevel(const std::string& name)
{
    std::unique_ptr<loader::Level> loader = loader::Level::createLoader(name, loader::Game::Unknown);
    if(!loader)
        return false;

    loader->load();

    TR_GenWorld(engine_world, loader);

    std::string buf = getLevelName(name);

    Console::instance().notify(SYSNOTE_LOADED_PC_LEVEL);
    Console::instance().notify(SYSNOTE_ENGINE_VERSION, static_cast<int>(loader->m_gameVersion), buf.c_str());
    Console::instance().notify(SYSNOTE_NUM_ROOMS, engine_world.rooms.size());

    return true;
}

int loadMap(const std::string& name)
{
    if(!boost::filesystem::is_regular_file(name))
    {
        Console::instance().warning(SYSWARN_FILE_NOT_FOUND, name.c_str());
        return 0;
    }

    gui::drawLoadScreen(0);

    engine_camera.setCurrentRoom( nullptr );

    render::renderer.hideSkyBox();
    render::renderer.resetWorld();

    Gameflow_Manager.setLevelPath(name);          // it is needed for "not in the game" levels or correct saves loading.

    gui::drawLoadScreen(50);

    engine_world.empty();
    engine_world.prepare();

    engine_lua.clean();

    engine_world.audioEngine.init();

    gui::drawLoadScreen(100);

    // Here we can place different platform-specific level loading routines.

    switch(getLevelFormat(name))
    {
        case LEVEL_FORMAT_PC:
            if(loadPCLevel(name) == false) return 0;
            break;

        case LEVEL_FORMAT_PSX:
            break;

        case LEVEL_FORMAT_DC:
            break;

        case LEVEL_FORMAT_OPENTOMB:
            break;

        default:
            break;
    }

    engine_world.id = 0;
    engine_world.name = nullptr;
    engine_world.type = 0;

    Game_Prepare();

    engine_lua.prepare();

    render::renderer.setWorld(&engine_world);

    gui::drawLoadScreen(1000);

    gui::fadeStart(gui::FaderType::LoadScreen, gui::FaderDir::In);
    gui::notifierStop();

    return 1;
}

int execCmd(const char *ch)
{
    std::vector<char> token(Console::instance().lineSize());
    world::RoomSector* sect;
    FILE *f;

    while(ch != nullptr)
    {
        const char *pch = ch;

        ch = script::MainEngine::parse_token(ch, token.data());
        if(!strcmp(token.data(), "help"))
        {
            for(int i = SYSNOTE_COMMAND_HELP1; i <= SYSNOTE_COMMAND_HELP15; i++)
            {
                Console::instance().notify(i);
            }
        }
        else if(!strcmp(token.data(), "goto"))
        {
            control_states.free_look = true;
            const auto x = script::MainEngine::parseFloat(&ch);
            const auto y = script::MainEngine::parseFloat(&ch);
            const auto z = script::MainEngine::parseFloat(&ch);
            render::renderer.camera()->setPosition({ x, y, z });
            return 1;
        }
        else if(!strcmp(token.data(), "save"))
        {
            ch = script::MainEngine::parse_token(ch, token.data());
            if(NULL != ch)
            {
                Game_Save(token.data());
            }
            return 1;
        }
        else if(!strcmp(token.data(), "load"))
        {
            ch = script::MainEngine::parse_token(ch, token.data());
            if(NULL != ch)
            {
                Game_Load(token.data());
            }
            return 1;
        }
        else if(!strcmp(token.data(), "exit"))
        {
            shutdown(0);
            return 1;
        }
        else if(!strcmp(token.data(), "cls"))
        {
            Console::instance().clean();
            return 1;
        }
        else if(!strcmp(token.data(), "spacing"))
        {
            ch = script::MainEngine::parse_token(ch, token.data());
            if(NULL == ch)
            {
                Console::instance().notify(SYSNOTE_CONSOLE_SPACING, Console::instance().spacing());
                return 1;
            }
            Console::instance().setLineInterval(std::stof(token.data()));
            return 1;
        }
        else if(!strcmp(token.data(), "showing_lines"))
        {
            ch = script::MainEngine::parse_token(ch, token.data());
            if(NULL == ch)
            {
                Console::instance().notify(SYSNOTE_CONSOLE_LINECOUNT, Console::instance().visibleLines());
                return 1;
            }
            else
            {
                const auto val = atoi(token.data());
                if((val >=2 ) && (val <= screen_info.h/Console::instance().lineHeight()))
                {
                    Console::instance().setVisibleLines(val);
                    Console::instance().setCursorY(screen_info.h - Console::instance().lineHeight() * Console::instance().visibleLines());
                }
                else
                {
                    Console::instance().warning(SYSWARN_INVALID_LINECOUNT);
                }
            }
            return 1;
        }
        else if(!strcmp(token.data(), "r_wireframe"))
        {
            render::renderer.toggleWireframe();
            return 1;
        }
        else if(!strcmp(token.data(), "r_points"))
        {
            render::renderer.toggleDrawPoints();
            return 1;
        }
        else if(!strcmp(token.data(), "r_coll"))
        {
            render::renderer.toggleDrawColl();
            return 1;
        }
        else if(!strcmp(token.data(), "r_normals"))
        {
            render::renderer.toggleDrawNormals();
            return 1;
        }
        else if(!strcmp(token.data(), "r_portals"))
        {
            render::renderer.toggleDrawPortals();
            return 1;
        }
        else if(!strcmp(token.data(), "r_room_boxes"))
        {
            render::renderer.toggleDrawRoomBoxes();
            return 1;
        }
        else if(!strcmp(token.data(), "r_boxes"))
        {
            render::renderer.toggleDrawBoxes();
            return 1;
        }
        else if(!strcmp(token.data(), "r_axis"))
        {
            render::renderer.toggleDrawAxis();
            return 1;
        }
        else if(!strcmp(token.data(), "r_allmodels"))
        {
            render::renderer.toggleDrawAllModels();
            return 1;
        }
        else if(!strcmp(token.data(), "r_dummy_statics"))
        {
            render::renderer.toggleDrawDummyStatics();
            return 1;
        }
        else if(!strcmp(token.data(), "r_skip_room"))
        {
            render::renderer.toggleSkipRoom();
            return 1;
        }
        else if(!strcmp(token.data(), "room_info"))
        {
            if(world::Room* r = render::renderer.camera()->getCurrentRoom())
            {
                sect = r->getSectorXYZ(render::renderer.camera()->getPosition());
                Console::instance().printf("ID = %d, x_sect = %d, y_sect = %d", r->getId(), static_cast<int>(r->m_sectors.shape()[0]), static_cast<int>(r->m_sectors.shape()[1]));
                if(sect)
                {
                    Console::instance().printf("sect(%d, %d), inpenitrable = %d, r_up = %d, r_down = %d",
                                               static_cast<int>(sect->index_x),
                                               static_cast<int>(sect->index_y),
                                               static_cast<int>(sect->ceiling == world::MeteringWallHeight || sect->floor == world::MeteringWallHeight),
                                               static_cast<int>(sect->sector_above != nullptr), static_cast<int>(sect->sector_below != nullptr));
                    for(uint32_t i = 0; i < sect->owner_room->m_staticMeshes.size(); i++)
                    {
                        Console::instance().printf("static[%d].object_id = %d", i, sect->owner_room->m_staticMeshes[i]->getId());
                    }
                    for(world::Object* object : sect->owner_room->m_objects)
                    {
                        if(world::Entity* e = dynamic_cast<world::Entity*>(object))
                        {
                            Console::instance().printf("object[entity](%d, %d, %d).object_id = %d", static_cast<int>(e->m_transform[3][0]), static_cast<int>(e->m_transform[3][1]), static_cast<int>(e->m_transform[3][2]), e->getId());
                        }
                    }
                }
            }
            return 1;
        }
        else if(!strcmp(token.data(), "xxx"))
        {
            f = fopen("ascII.txt", "r");
            if(f)
            {
                fseek(f, 0, SEEK_END);
                auto size = ftell(f);
                std::vector<char> buf(size + 1);

                fseek(f, 0, SEEK_SET);
                fread(buf.data(), size, sizeof(char), f);
                buf[size] = 0;
                fclose(f);
                Console::instance().clean();
                Console::instance().addText(buf.data(), gui::FontStyle::ConsoleInfo);
            }
            else
            {
                Console::instance().addText("Not avaliable =(", gui::FontStyle::ConsoleWarning);
            }
            return 1;
        }
        else if(token[0])
        {
            Console::instance().addLine(pch, gui::FontStyle::ConsoleEvent);
            try
            {
                engine_lua.doString(pch);
            }
            catch(lua::RuntimeError& error)
            {
                Console::instance().addLine(error.what(), gui::FontStyle::ConsoleWarning);
            }
            catch(lua::LoadError& error)
            {
                Console::instance().addLine(error.what(), gui::FontStyle::ConsoleWarning);
            }
            return 0;
        }
    }

    return 0;
}

void initConfig(const std::string& filename)
{
    initDefaultGlobals();

    if(boost::filesystem::is_regular_file(filename))
    {
        script::ScriptEngine state;
        state.registerC("bind", &script::MainEngine::bindKey);                             // get and set key bindings
        try
        {
            state.doFile(filename);
        }
        catch(lua::RuntimeError& error)
        {
            BOOST_LOG_TRIVIAL(error) << error.what();
            return;
        }
        catch(lua::LoadError& error)
        {
            BOOST_LOG_TRIVIAL(error) << error.what();
            return;
        }

        state.parseScreen(screen_info);
        state.parseRender(render::renderer.settings());
        state.parseAudio(engine_world.audioEngine.settings());
        state.parseConsole(Console::instance());
        state.parseControls(control_mapper);
        state.parseSystem(system_settings);
    }
    else
    {
        BOOST_LOG_TRIVIAL(error) << "Could not find " << filename;
    }
}

int engine_lua_fputs(const char *str, FILE* /*f*/)
{
    Console::instance().addText(str, gui::FontStyle::ConsoleNotify);
    return static_cast<int>(strlen(str));
}

int engine_lua_fprintf(FILE *f, const char *fmt, ...)
{
    va_list argptr;
    char buf[4096];
    int ret;

    // Create string
    va_start(argptr, fmt);
    ret = vsnprintf(buf, 4096, fmt, argptr);
    va_end(argptr);

    // Write it to target file
    fwrite(buf, 1, ret, f);

    // Write it to console, too (if it helps) und
    Console::instance().addText(buf, gui::FontStyle::ConsoleNotify);

    return ret;
}

int engine_lua_printf(const char *fmt, ...)
{
    va_list argptr;
    char buf[4096];
    int ret;

    va_start(argptr, fmt);
    ret = vsnprintf(buf, 4096, fmt, argptr);
    va_end(argptr);

    Console::instance().addText(buf, gui::FontStyle::ConsoleNotify);

    return ret;
}

btScalar BtEngineClosestRayResultCallback::addSingleResult(btCollisionWorld::LocalRayResult& rayResult, bool normalInWorldSpace)
{
    const world::Object* c1 = static_cast<const world::Object*>(rayResult.m_collisionObject->getUserPointer());

    if(c1 && (c1 == m_object || (m_skip_ghost && c1->getCollisionType() == world::CollisionType::Ghost)))
    {
        return 1.0;
    }

    const world::Room* r0 = m_object ? m_object->getRoom() : nullptr;
    const world::Room* r1 = c1 ? c1->getRoom() : nullptr;

    if(!r0 || !r1)
    {
        return ClosestRayResultCallback::addSingleResult(rayResult, normalInWorldSpace);
    }

    if(r0 && r1)
    {
        if(r0->isInNearRoomsList(*r1))
        {
            return ClosestRayResultCallback::addSingleResult(rayResult, normalInWorldSpace);
        }
        else
        {
            return 1.0;
        }
    }

    return 1.0;
}

btScalar BtEngineClosestConvexResultCallback::addSingleResult(btCollisionWorld::LocalConvexResult& convexResult, bool normalInWorldSpace)
{
    const world::Room* r0 = m_object ? m_object->getRoom() : nullptr;
    const world::Object* c1 = static_cast<const world::Object*>(convexResult.m_hitCollisionObject->getUserPointer());
    const world::Room* r1 = c1 ? c1->getRoom() : nullptr;

    if(c1 && (c1 == m_object || (m_skip_ghost && c1->getCollisionType() == world::CollisionType::Ghost)))
    {
        return 1.0;
    }

    if(!r0 || !r1)
    {
        return ClosestConvexResultCallback::addSingleResult(convexResult, normalInWorldSpace);
    }

    if(r0 && r1)
    {
        if(r0->isInNearRoomsList(*r1))
        {
            return ClosestConvexResultCallback::addSingleResult(convexResult, normalInWorldSpace);
        }
        else
        {
            return 1.0;
        }
    }

    return 1.0;
}

} // namespace engine