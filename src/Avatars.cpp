#include "Avatars.h"

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

            // One per id, so two players are told apart at a glance.
            const uint32_t TINTS[8] =
            {
                0xFFFFFFFF, 0xFF80C0FF, 0xFF80FF90, 0xFFFFD070,
                0xFFFF90C0, 0xFF90FFF0, 0xFFC090FF, 0xFFFF8060,
            };

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

            // Row vectors, the way the game holds them: translation in 12..14.
            void Multiply(const float* a, const float* b, float* out)
            {
                for (int row = 0; row < 4; ++row)
                {
                    for (int column = 0; column < 4; ++column)
                    {
                        out[row * 4 + column] =
                            a[row * 4 + 0] * b[column] +
                            a[row * 4 + 1] * b[4 + column] +
                            a[row * 4 + 2] * b[8 + column] +
                            a[row * 4 + 3] * b[12 + column];
                    }
                }
            }

            void Place(const Body& body, const float* view, float* out)
            {
                const float turn = body.facing;
                const float sine = sinf(turn);
                const float cosine = cosf(turn);

                const float world[16] =
                {
                    cosine, 0.0f, -sine, 0.0f,
                    0.0f,   1.0f,  0.0f, 0.0f,
                    sine,   0.0f, cosine, 0.0f,
                    body.position[0], body.position[1], body.position[2], 1.0f,
                };

                Multiply(world, view, out);
            }

            void Queue();

            void __cdecl OnPlayerDrawn(const AmodModelDrawInfo* info, void*)
            {
                if (info != nullptr)
                {
                    memcpy(g_theirs, info->matrix, sizeof(g_theirs));
                    g_seenTheirs = true;
                }

                if (g_pending)
                {
                    g_pending = false;
                    Queue();
                }
            }

            AmodModelId Model();

            void Queue()
            {
                g_tally = Drawn{};

                const AmodModelId model = Model();
                if (model == AMOD_INVALID_MODEL)
                {
                    g_tally.noModel = true;
                    return;
                }

                float view[16] = {};
                if (!Mmd::View(view))
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

                    AmodModelDraw draw = {};
                    draw.structSize = sizeof(draw);
                    draw.model = model;
                    draw.texture = g_skin;
                    draw.flags = AMOD_DRAW_HAS_MATRIX;
                    Place(body, view, draw.matrix);
                    draw.frame = g_holdFrame >= 0.0f ? g_holdFrame : body.frame;
                    draw.tint = TINTS[body.id % 8];

                    if (Mmd::Enqueue(draw))
                    {
                        ++g_tally.queued;
                    }
                    else
                    {
                        ++g_tally.refused;
                    }
                }
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

            AmodAnimState anim = {};
            if (Mmd::GetEntityAnim(World::PlayerEntity(), anim))
            {
                out.frame = static_cast<float>(anim.frameA) + static_cast<float>(anim.frameB - anim.frameA) * anim.blend;
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

            // Nothing is drawn until the game draws him, so the model has to be in hand by then.
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

        void Forget()
        {
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
