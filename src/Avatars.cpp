#include "Avatars.h"

// Before ForgeApi, which hands out the device but declares nothing to call on it.
#include <d3d8.h>

#include <ForgeApi.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

using namespace Amod;

namespace Mp
{
    namespace Avatars
    {
        namespace
        {
            const char* const MODEL_PATH = "models/player.mmd";
            const char* const SKIN_PATH = "textures/player_skin.jpg";

            float g_holdFrame = -1.0f;

            AmodModelId g_model = AMOD_INVALID_MODEL;
            uint32_t g_skin = 0;
            AmodToken g_watch = AMOD_INVALID_TOKEN;

            // The matrix the game itself draws him with, for comparing ours against.
            float g_theirs[16] = {};
            bool g_seenTheirs = false;

            // This frame's bodies, put here by the session and drawn from the callback below.
            const int MAX_SHOWN = 8;

            Body g_shown[MAX_SHOWN] = {};
            int g_shownCount = 0;
            uint16_t g_shownRoom = 0;
            bool g_pending = false;
            Drawn g_tally = {};

            void Place(const Body& body, D3DMATRIX& out)
            {
                const float sine = sinf(body.facing);
                const float cosine = cosf(body.facing);

                memset(&out, 0, sizeof(out));

                out._11 = cosine;
                out._13 = -sine;
                out._22 = 1.0f;
                out._31 = sine;
                out._33 = cosine;
                out._41 = body.position[0];
                out._42 = body.position[1];
                out._43 = body.position[2];
                out._44 = 1.0f;
            }

            // The camera the level is drawn with, taken while it is being drawn: by the end of the frame the 2D pass has its own.
            D3DMATRIX g_view = {};
            D3DMATRIX g_projection = {};
            D3DVIEWPORT8 g_viewport = {};
            bool g_seenView = false;

            void SeeView()
            {
                IDirect3DDevice8* const device = Render::Device();

                g_seenView = device != nullptr &&
                    device->GetTransform(D3DTS_VIEW, &g_view) == D3D_OK &&
                    device->GetTransform(D3DTS_PROJECTION, &g_projection) == D3D_OK &&
                    device->GetViewport(&g_viewport) == D3D_OK;
            }

            void DrawThem();

            void __cdecl OnPlayerDrawn(const AmodModelDrawInfo* info, void*)
            {
                if (info != nullptr)
                {
                    memcpy(g_theirs, info->matrix, sizeof(g_theirs));
                    g_seenTheirs = true;
                }

                SeeView();

                if (g_pending)
                {
                    g_pending = false;
                    DrawThem();
                }
            }

            AmodModelId Model();

            void DrawThem()
            {
                g_tally = Drawn{};

                const AmodModelId model = Model();
                if (model == AMOD_INVALID_MODEL)
                {
                    g_tally.noModel = true;
                    return;
                }

                IDirect3DDevice8* const device = Render::Device();
                if (device == nullptr)
                {
                    g_tally.noView = true;
                    return;
                }

                AmodModel about = {};
                const int frames = Mmd::Get(model, about) ? about.frameCount : 0;

                D3DMATRIX was = {};
                if (device->GetTransform(D3DTS_WORLD, &was) != D3D_OK)
                {
                    g_tally.noView = true;
                    return;
                }

                for (int at = 0; at < g_shownCount; ++at)
                {
                    const Body& body = g_shown[at];

                    if (body.room != g_shownRoom)
                    {
                        ++g_tally.elsewhere;
                        continue;
                    }

                    D3DMATRIX ours = {};
                    Place(body, ours);
                    device->SetTransform(D3DTS_WORLD, &ours);

                    // The two the game itself is blending, not a pair worked out from one
                    // number: the frame after a movement's last is the start of another movement.
                    int first = body.frameA;
                    int second = body.frameB;
                    float blend = static_cast<float>(body.blend) / 255.0f;

                    if (g_holdFrame >= 0.0f)
                    {
                        first = static_cast<int>(g_holdFrame);
                        second = first;
                        blend = 0.0f;
                    }

                    if (frames > 0)
                    {
                        first = first % frames;
                        second = second % frames;
                    }

                    if (Mmd::DrawFrames(model, first, second, blend))
                    {
                        ++g_tally.queued;
                    }
                    else
                    {
                        ++g_tally.refused;
                    }
                }

                device->SetTransform(D3DTS_WORLD, &was);
            }

            // Models belong to the level, so the id goes stale on the way out of one.
            AmodModelId Model()
            {
                AmodModel info = {};

                if (g_model != AMOD_INVALID_MODEL && Mmd::Get(g_model, info))
                {
                    return g_model;
                }

                Forget();

                g_model = Mmd::Find(MODEL_PATH);
                if (g_model == AMOD_INVALID_MODEL)
                {
                    return AMOD_INVALID_MODEL;
                }

                g_skin = Mmd::Texture(SKIN_PATH);
                g_watch = Mmd::OnDraw(g_model, &OnPlayerDrawn);

                if (Mmd::Get(g_model, info))
                {
                    Log(AMOD_LOG_INFO, "%s: %d frame(s), %d vertices, extents %.2f %.2f %.2f",
                        MODEL_PATH, info.frameCount, info.vertexCount,
                        info.extents[0], info.extents[1], info.extents[2]);
                }

                return g_model;
            }
        }

        bool ReadLocal(Body& out)
        {
            out = Body{};

            if (!Player::IsHere())
            {
                return false;
            }

            AmodPlayer player = {};
            if (!Player::Get(player))
            {
                return false;
            }

            out.room = static_cast<uint16_t>(World::CurrentRoom());
            memcpy(out.position, player.position, sizeof(out.position));
            out.facing = player.facing;
            out.state = static_cast<uint8_t>(player.state);
            out.subState = static_cast<uint8_t>(player.subState);
            out.flags = player.isDead != 0 ? BODY_DEAD : 0;

            if (g_seenTheirs)
            {
                out.facing = atan2f(-g_theirs[2], g_theirs[0]);
            }

            AmodAnimState anim = {};
            if (Mmd::GetEntityAnim(World::PlayerEntity(), anim))
            {
                const float blend = anim.blend < 0.0f ? 0.0f : (anim.blend > 1.0f ? 1.0f : anim.blend);

                out.frameA = static_cast<uint16_t>(anim.frameA < 0 ? 0 : anim.frameA);
                out.frameB = static_cast<uint16_t>(anim.frameB < 0 ? 0 : anim.frameB);
                out.blend = static_cast<uint8_t>(blend * 255.0f + 0.5f);
            }

            return true;
        }

        void Show(const Body* bodies, int count, uint16_t room)
        {
            g_shownCount = 0;
            g_shownRoom = room;

            for (int at = 0; at < count && g_shownCount < MAX_SHOWN; ++at)
            {
                g_shown[g_shownCount++] = bodies[at];
            }

            g_pending = true;

            Model();
        }

        Drawn Tally()
        {
            return g_tally;
        }

        void HoldFrame(float frame)
        {
            g_holdFrame = frame;
        }

        float HeldFrame()
        {
            return g_holdFrame;
        }

        int ShownCount()
        {
            return g_shownCount;
        }

        const Body* ShownAt(int index)
        {
            return index >= 0 && index < g_shownCount ? &g_shown[index] : nullptr;
        }

        uint16_t ShownRoom()
        {
            return g_shownRoom;
        }

        bool Project(const float world[3], float& x, float& y)
        {
            int wide = 0;
            int tall = 0;

            Render::Viewport(&wide, &tall);

            if (!g_seenView || wide <= 0 || tall <= 0)
            {
                return false;
            }

            // Row vectors, as Direct3D multiplies them: world, then view, then projection.
            const D3DMATRIX& v = g_view;
            const D3DMATRIX& p = g_projection;

            const float ex = world[0] * v._11 + world[1] * v._21 + world[2] * v._31 + v._41;
            const float ey = world[0] * v._12 + world[1] * v._22 + world[2] * v._32 + v._42;
            const float ez = world[0] * v._13 + world[1] * v._23 + world[2] * v._33 + v._43;

            const float cx = ex * p._11 + ey * p._21 + ez * p._31 + p._41;
            const float cy = ex * p._12 + ey * p._22 + ez * p._32 + p._42;
            const float cw = ex * p._14 + ey * p._24 + ez * p._34 + p._44;

            if (cw <= 0.0001f)
            {
                return false;
            }

            const float nx = cx / cw;
            const float ny = cy / cw;

            if (nx < -1.2f || nx > 1.2f || ny < -1.2f || ny > 1.2f)
            {
                return false;
            }

            const float px = g_viewport.X + (nx + 1.0f) * 0.5f * g_viewport.Width;
            const float py = g_viewport.Y + (1.0f - ny) * 0.5f * g_viewport.Height;

            x = px * AMOD_MENU_WIDE / static_cast<float>(wide);
            y = py * AMOD_MENU_TALL / static_cast<float>(tall);
            return true;
        }

        void Forget()
        {
            g_seenView = false;

            if (g_watch != AMOD_INVALID_TOKEN)
            {
                Mmd::Cancel(g_watch);
                g_watch = AMOD_INVALID_TOKEN;
            }

            g_model = AMOD_INVALID_MODEL;
            g_skin = 0;
            g_seenTheirs = false;
        }

        void Report(void (*line)(const char* text))
        {
            if (line == nullptr)
            {
                return;
            }

            char text[256];

            if (g_model == AMOD_INVALID_MODEL)
            {
                line("mp: no player model yet; start a level\n");
                return;
            }

            AmodModel info = {};
            Mmd::Get(g_model, info);

            sprintf_s(text, "mp: %s, %d frame(s), skin %s\n",
                      MODEL_PATH, info.frameCount, g_skin != 0 ? "found" : "MISSING");
            line(text);

            if (!g_seenTheirs)
            {
                line("mp: the game has not drawn him yet\n");
                return;
            }

            sprintf_s(text, "mp: game matrix x %.3f %.3f %.3f | y %.3f %.3f %.3f\n",
                      g_theirs[0], g_theirs[1], g_theirs[2],
                      g_theirs[4], g_theirs[5], g_theirs[6]);
            line(text);

            sprintf_s(text, "mp: game puts him at %.2f %.2f %.2f\n",
                      g_theirs[12], g_theirs[13], g_theirs[14]);

            line(text);
        }
    }
}
