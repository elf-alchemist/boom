// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// $Id: p_enemy.c,v 1.23 1998/08/13 15:27:26 jim Exp $
//
//  BOOM, a modified and improved DOOM engine
//  Copyright (C) 1999 by
//  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 
//  02111-1307, USA.
//
// DESCRIPTION:
//      Enemy thinking, AI.
//      Action Pointer Functions
//      that are associated with states/frames.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: p_enemy.c,v 1.23 1998/08/13 15:27:26 jim Exp $";

#include "doomstat.h"
#include "m_random.h"
#include "r_main.h"
#include "p_maputl.h"
#include "p_map.h"
#include "p_setup.h"
#include "p_spec.h"
#include "s_sound.h"
#include "sounds.h"
#include "p_inter.h"
#include "g_game.h"
#include "p_enemy.h"

typedef enum {
  DI_EAST,
  DI_NORTHEAST,
  DI_NORTH,
  DI_NORTHWEST,
  DI_WEST,
  DI_SOUTHWEST,
  DI_SOUTH,
  DI_SOUTHEAST,
  DI_NODIR,
  NUMDIRS
} dirtype_t;

//
// P_NewChaseDir related LUT.
//
dirtype_t opposite[] = {
  DI_WEST, DI_SOUTHWEST, DI_SOUTH, DI_SOUTHEAST,
  DI_EAST, DI_NORTHEAST, DI_NORTH, DI_NORTHWEST, DI_NODIR
};

dirtype_t diags[] = {
  DI_NORTHWEST, DI_NORTHEAST, DI_SOUTHWEST, DI_SOUTHEAST
};

void P_ZBumpCheck(mobj_t *);                                        // phares
void A_Fall(mobj_t *actor);

//
// ENEMY THINKING
// Enemies are allways spawned
// with targetplayer = -1, threshold = 0
// Most monsters are spawned unaware of all players,
// but some can be made preaware
//

//
// Called by P_NoiseAlert.
// Recursively traverse adjacent sectors,
// sound blocking lines cut off traversal.
//
// killough 5/5/98: reformatted, cleaned up

void P_RecursiveSound(sector_t *sec, int soundblocks, mobj_t *soundtarget)
{
  int i;

  // wake up all monsters in this sector
  if (sec->validcount == validcount && sec->soundtraversed <= soundblocks+1)
    return;             // already flooded

  sec->validcount = validcount;
  sec->soundtraversed = soundblocks+1;
  sec->soundtarget = soundtarget;

  for (i=0; i<sec->linecount; i++)
    {
      sector_t *other;
      line_t *check = sec->lines[i];

      if (!(check->flags & ML_TWOSIDED))
        continue;

      P_LineOpening(check);

      if (openrange <= 0)
        continue;       // closed door

      other=sides[check->sidenum[sides[check->sidenum[0]].sector==sec]].sector;

      if (!(check->flags & ML_SOUNDBLOCK))
        P_RecursiveSound(other, soundblocks, soundtarget);
      else
        if (!soundblocks)
          P_RecursiveSound(other, 1, soundtarget);
    }
}

//
// P_NoiseAlert
// If a monster yells at a player,
// it will alert other monsters to the player.
//
void P_NoiseAlert(mobj_t *target, mobj_t *emitter)
{
  validcount++;
  P_RecursiveSound(emitter->subsector->sector, 0, target);
}

//
// P_CheckMeleeRange
//
boolean P_CheckMeleeRange(mobj_t *actor)
{
  mobj_t  *pl;
  fixed_t dist;

  if (!actor->target)
    return false;

  pl = actor->target;
  dist = P_AproxDistance(pl->x-actor->x, pl->y-actor->y);

  if (dist >= MELEERANGE-20*FRACUNIT+pl->info->radius)
    return false;

  if (!P_CheckSight(actor, actor->target))
    return false;

  return true;
}

//
// P_CheckMissileRange
//
boolean P_CheckMissileRange(mobj_t *actor)
{
  fixed_t dist;

  if (!P_CheckSight(actor, actor->target))
    return false;

  if (actor->flags & MF_JUSTHIT)
    {      // the target just hit the enemy, so fight back!
      actor->flags &= ~MF_JUSTHIT;
      return true;
    }

  if (actor->reactiontime)
    return false;       // do not attack yet

  // OPTIMIZE: get this from a global checksight
  dist = P_AproxDistance ( actor->x-actor->target->x,
                           actor->y-actor->target->y) - 64*FRACUNIT;

  if (!actor->info->meleestate)
    dist -= 128*FRACUNIT;       // no melee attack, so fire more

  dist >>= 16;

  if (actor->type == MT_VILE)
    if (dist > 14*64)
      return false;     // too far away


  if (actor->type == MT_UNDEAD)
    {
      if (dist < 196)
        return false;   // close for fist attack
      dist >>= 1;
    }

  if (actor->type == MT_CYBORG ||
      actor->type == MT_SPIDER ||
      actor->type == MT_SKULL)
    dist >>= 1;

  if (dist > 200)
    dist = 200;

  if (actor->type == MT_CYBORG && dist > 160)
    dist = 160;

  if (P_Random(pr_missrange) < dist)
    return false;

  return true;
}

//
// P_Move
// Move in the current direction,
// returns false if the move is blocked.
//

fixed_t xspeed[8] = {FRACUNIT,47000,0,-47000,-FRACUNIT,-47000,0,47000};
fixed_t yspeed[8] = {0,47000,FRACUNIT,47000,0,-47000,-FRACUNIT,-47000};

// 1/11/98 killough: Limit removed on special lines crossed
extern  line_t **spechit;          // New code -- killough
extern  int    numspechit;

boolean P_Move(mobj_t *actor)
{
  fixed_t tryx;
  fixed_t tryy;
  boolean try_ok;
  boolean good;

  if (actor->movedir == DI_NODIR)
    return false;

  if ((unsigned)actor->movedir >= 8)
    I_Error ("Weird actor->movedir!");

  tryx = actor->x + actor->info->speed*xspeed[actor->movedir];
  tryy = actor->y + actor->info->speed*yspeed[actor->movedir];

  // killough 3/15/98: don't jump over dropoffs:
  try_ok = P_TryMove(actor, tryx, tryy, false);

  if (!try_ok)
    {      // open any specials
      if (actor->flags & MF_FLOAT && floatok)
        {
          if (actor->z < tmfloorz)          // must adjust height
            actor->z += FLOATSPEED;
          else
            actor->z -= FLOATSPEED;

          actor->flags |= MF_INFLOAT;
          return true;
        }

      if (!numspechit)
        return false;

      actor->movedir = DI_NODIR;

      // if the special is not a door that can be opened, return false
      for (good = false; numspechit--; )
        if (P_UseSpecialLine(actor, spechit[numspechit], 0))
          good = true;
      return good && (compatibility || (P_Random(pr_trywalk)&3)); //jff 8/13/98
    }                                          // 1 in 4 try a different dir
  else                                         // avoid stuck in doorway
    actor->flags &= ~MF_INFLOAT;

  if (!(actor->flags & MF_FLOAT))
    actor->z = actor->floorz;

  return true;
}

//
// TryWalk
// Attempts to move actor on
// in its current (ob->moveangle) direction.
// If blocked by either a wall or an actor
// returns FALSE
// If move is either clear or blocked only by a door,
// returns TRUE and sets...
// If a door is in the way,
// an OpenDoor call is made to start it opening.
//

boolean P_TryWalk(mobj_t *actor)
{
  if (!P_Move(actor))
    return false;
  actor->movecount = P_Random(pr_trywalk)&15;
  return true;
}

void P_NewChaseDir(mobj_t *actor)
{
  fixed_t     deltax;
  fixed_t     deltay;
  dirtype_t   d[3];
  dirtype_t   olddir;
  int         tdir;
  dirtype_t   turnaround;

  if (!actor->target)
    I_Error ("P_NewChaseDir: called with no target");

  olddir = actor->movedir;
  turnaround=opposite[olddir];

  deltax = actor->target->x - actor->x;
  deltay = actor->target->y - actor->y;

  if (deltax>10*FRACUNIT)
    d[1]= DI_EAST;
  else
    if (deltax<-10*FRACUNIT)
      d[1]= DI_WEST;
    else
      d[1]=DI_NODIR;

  if (deltay<-10*FRACUNIT)
    d[2]= DI_SOUTH;
  else
    if (deltay>10*FRACUNIT)
      d[2]= DI_NORTH;
    else
      d[2]=DI_NODIR;

  // try direct route
  if (d[1] != DI_NODIR && d[2] != DI_NODIR)
    {
      actor->movedir = diags[((deltay<0)<<1)+(deltax>0)];
      if (actor->movedir != turnaround && P_TryWalk(actor))
        return;
    }

  // try other directions
  if (P_Random(pr_newchase) > 200 || abs(deltay)>abs(deltax))
    {
      tdir=d[1];
      d[1]=d[2];
      d[2]=tdir;
    }

  if (d[1]==turnaround)
    d[1]=DI_NODIR;
  if (d[2]==turnaround)
    d[2]=DI_NODIR;

  if (d[1]!=DI_NODIR)
    {
      actor->movedir = d[1];
      if (P_TryWalk(actor))         // either moved forward or attacked
        return;
    }

  if (d[2]!=DI_NODIR)
    {
      actor->movedir = d[2];
      if (P_TryWalk(actor))
        return;
    }

  // there is no direct path to the player, so pick another direction.

  if (olddir!=DI_NODIR)
    {
      actor->movedir = olddir;
      if (P_TryWalk(actor))
        return;
    }

  // randomly determine direction of search

  if (P_Random(pr_newchasedir)&1)
    {
      for (tdir=DI_EAST; tdir<=DI_SOUTHEAST; tdir++)
        if (tdir!=turnaround)
          {
            actor->movedir =tdir;
            if (P_TryWalk(actor))
              return;
          }
    }
  else
    {
      for (tdir=DI_SOUTHEAST; tdir != DI_EAST-1; tdir--)
        if (tdir!=turnaround)
          {
            actor->movedir =tdir;
            if (P_TryWalk(actor))
              return;
          }
    }

  if (turnaround != DI_NODIR)
    {
      actor->movedir =turnaround;
      if (P_TryWalk(actor))
        return;
    }

  actor->movedir = DI_NODIR;    // can not move
}

//
// P_LookForPlayers
// If allaround is false, only look 180 degrees in front.
// Returns true if a player is targeted.
//

boolean P_LookForPlayers(mobj_t *actor, boolean allaround)
{
  int      c = 0;
  int      stop;
  player_t *player;
  int smartypants = !demo_compatibility && monsters_remember; // killough

  // Change mask of 3 to (MAXPLAYERS-1) -- killough 2/15/98:
  stop = (actor->lastlook-1)&(MAXPLAYERS-1);

  for (;; actor->lastlook = (actor->lastlook+1)&(MAXPLAYERS-1))
    {
      if (!playeringame[actor->lastlook])
        continue;

      if (c++ == (smartypants ? MAXPLAYERS : 2) // killough 2/15/98
          || actor->lastlook == stop)  // done looking
        break;     //  exit loop instead of function -- killough 2/15/98

      player = &players[actor->lastlook];

      if (player->health <= 0)
        continue;               // dead

      if (!P_CheckSight (actor, player->mo))
        continue;               // out of sight

      if (!allaround)
        {
          angle_t an = R_PointToAngle2 (actor->x, actor->y, player->mo->x,
                                        player->mo->y) - actor->angle;

          if (an > ANG90 && an < ANG270)
            {
              fixed_t dist = P_AproxDistance (player->mo->x - actor->x,
                                              player->mo->y - actor->y);

              // if real close, react anyway                        // phares
                                                                    // phares
              if (dist > MELEERANGE)                                // phares
                continue;       // behind back

            }
        }
      actor->target = player->mo;
      return true;
    }

  // Use last known enemy if no players sighted -- killough 2/15/98:

  if (smartypants)
    if (actor->lastenemy && actor->lastenemy->health > 0)
      {
        actor->target = actor->lastenemy;
        actor->lastenemy = NULL;
        return true;
      }

  return false;
}

//
// A_KeenDie
// DOOM II special, map 32.
// Uses special tag 666.
//
void A_KeenDie(mobj_t* mo)
{
  thinker_t *th;
  line_t   junk;

  A_Fall(mo);

  // scan the remaining thinkers to see if all Keens are dead

  for (th = thinkercap.next ; th != &thinkercap ; th=th->next)
    if (th->function.acp1 == (actionf_p1)P_MobjThinker)
      {
        mobj_t *mo2 = (mobj_t *) th;
        if (mo2 != mo && mo2->type == mo->type && mo2->health > 0)
          return;                           // other Keen not dead
      }

  junk.tag = 666;
  EV_DoDoor(&junk,open);
}


//
// ACTION ROUTINES
//

//
// A_Look
// Stay in state until a player is sighted.
//

void A_Look(mobj_t *actor)
{
  mobj_t *targ = actor->subsector->sector->soundtarget;
  actor->threshold = 0; // any shot will wake up

  if (targ && targ->flags & MF_SHOOTABLE)
    {
      actor->target = targ;
      if (!(actor->flags & MF_AMBUSH) || P_CheckSight(actor, actor->target))
        goto seeyou;
    }

  if (!P_LookForPlayers(actor, false))
    return;

  // go into chase state

seeyou:

  if (actor->info->seesound)
    {
      int sound;
      switch (actor->info->seesound)
        {
        case sfx_posit1:
        case sfx_posit2:
        case sfx_posit3:
          sound = sfx_posit1+P_Random(pr_see)%3;
          break;

        case sfx_bgsit1:
        case sfx_bgsit2:
          sound = sfx_bgsit1+P_Random(pr_see)%2;
          break;

        default:
          sound = actor->info->seesound;
          break;
        }
      if (actor->type==MT_SPIDER || actor->type == MT_CYBORG)
        S_StartSound(NULL, sound);          // full volume
      else
        S_StartSound(actor, sound);
    }
  P_SetMobjState(actor, actor->info->seestate);
}

//
// A_Chase
// Actor has a melee attack,
// so it tries to close as fast as possible
//

void A_Chase(mobj_t *actor)
{
  if (actor->reactiontime)
    actor->reactiontime--;

  // modify target threshold
  if (actor->threshold)
    if (!actor->target || actor->target->health <= 0)
      actor->threshold = 0;
    else
      actor->threshold--;

    // turn towards movement direction if not there yet
  if (actor->movedir < 8)
    {
      int delta = (actor->angle &= (7<<29)) - (actor->movedir << 29);
      if (delta > 0)
        actor->angle -= ANG90/2;
      else
        if (delta < 0)
          actor->angle += ANG90/2;
    }

  if (!actor->target || !(actor->target->flags&MF_SHOOTABLE))
    {
      if (P_LookForPlayers(actor,true))    // look for a new target
        return;                            // got a new target
      P_SetMobjState(actor, actor->info->spawnstate);
      return;
    }

  // do not attack twice in a row
  if (actor->flags & MF_JUSTATTACKED)
    {
      actor->flags &= ~MF_JUSTATTACKED;
      if (gameskill != sk_nightmare && !fastparm)
        P_NewChaseDir(actor);
      return;
    }

  // check for melee attack
  if (actor->info->meleestate && P_CheckMeleeRange(actor))
    {
      if (actor->info->attacksound)
        S_StartSound(actor, actor->info->attacksound);
      P_SetMobjState(actor, actor->info->meleestate);
      return;
    }

  // check for missile attack
  if (actor->info->missilestate)
    if (!(gameskill < sk_nightmare && !fastparm && actor->movecount))
      if (P_CheckMissileRange(actor))
        {
          P_SetMobjState(actor, actor->info->missilestate);
          actor->flags |= MF_JUSTATTACKED;
          return;
        }

  if (netgame && !actor->threshold && !P_CheckSight(actor, actor->target))
    if (P_LookForPlayers(actor,true))
      return; // got a new target

  // chase towards player
  if (--actor->movecount<0 || !P_Move(actor))
    P_NewChaseDir(actor);

  // make active sound
  if (actor->info->activesound && P_Random(pr_see)<3)
    S_StartSound(actor, actor->info->activesound);
}

//
// A_FaceTarget
//
void A_FaceTarget(mobj_t *actor)
{
  if (!actor->target)
    return;
  actor->flags &= ~MF_AMBUSH;
  actor->angle = R_PointToAngle2(actor->x, actor->y,
                                 actor->target->x, actor->target->y);
  if (actor->target->flags & MF_SHADOW)
    { // killough 5/5/98: remove dependence on order of evaluation:
      int t = P_Random(pr_facetarget);
      actor->angle += (t-P_Random(pr_facetarget))<<21;
    }
}

//
// A_PosAttack
//

void A_PosAttack(mobj_t *actor)
{
  int angle, damage, slope, t;

  if (!actor->target)
    return;
  A_FaceTarget(actor);
  angle = actor->angle;
  slope = P_AimLineAttack(actor, angle, MISSILERANGE);
  S_StartSound(actor, sfx_pistol);

  // killough 5/5/98: remove dependence on order of evaluation:
  t = P_Random(pr_posattack);
  angle += (t - P_Random(pr_posattack))<<20;
  damage = (P_Random(pr_posattack)%5 + 1)*3;
  P_LineAttack(actor, angle, MISSILERANGE, slope, damage);
}

void A_SPosAttack(mobj_t* actor)
{
  int i, bangle, slope;

  if (!actor->target)
    return;
  S_StartSound(actor, sfx_shotgn);
  A_FaceTarget(actor);
  bangle = actor->angle;
  slope = P_AimLineAttack(actor, bangle, MISSILERANGE);
  for (i=0; i<3; i++)
    {  // killough 5/5/98: remove dependence on order of evaluation:
      int t = P_Random(pr_sposattack);
      int angle = bangle + ((t - P_Random(pr_sposattack))<<20);
      int damage = ((P_Random(pr_sposattack)%5)+1)*3;
      P_LineAttack(actor, angle, MISSILERANGE, slope, damage);
    }
}

void A_CPosAttack(mobj_t *actor)
{
  int angle, bangle, damage, slope, t;

  if (!actor->target)
    return;
  S_StartSound(actor, sfx_shotgn);
  A_FaceTarget(actor);
  bangle = actor->angle;
  slope = P_AimLineAttack(actor, bangle, MISSILERANGE);

  // killough 5/5/98: remove dependence on order of evaluation:
  t = P_Random(pr_cposattack);
  angle = bangle + ((t - P_Random(pr_cposattack))<<20);
  damage = ((P_Random(pr_cposattack)%5)+1)*3;
  P_LineAttack(actor, angle, MISSILERANGE, slope, damage);
}

void A_CPosRefire(mobj_t *actor)
{
  // keep firing unless target got out of sight
  A_FaceTarget(actor);

  if (P_Random(pr_cposrefire) < 40)
    return;

  if (!actor->target || actor->target->health <= 0
      || !P_CheckSight(actor, actor->target))
    P_SetMobjState(actor, actor->info->seestate);
}

void A_SpidRefire(mobj_t* actor)
{
  // keep firing unless target got out of sight
  A_FaceTarget(actor);

  if (P_Random(pr_spidrefire) < 10)
    return;

  if (!actor->target || actor->target->health <= 0
      || !P_CheckSight(actor, actor->target))
    P_SetMobjState(actor, actor->info->seestate);
}

void A_BspiAttack(mobj_t *actor)
{
  if (!actor->target)
    return;
  A_FaceTarget(actor);
  P_SpawnMissile(actor, actor->target, MT_ARACHPLAZ);  // launch a missile
}

//
// A_TroopAttack
//

void A_TroopAttack(mobj_t *actor)
{
  if (!actor->target)
    return;
  A_FaceTarget(actor);
  if (P_CheckMeleeRange(actor))
    {
      int damage;
      S_StartSound(actor, sfx_claw);
      damage = (P_Random(pr_troopattack)%8+1)*3;
      P_DamageMobj(actor->target, actor, actor, damage);
      return;
    }
  P_SpawnMissile(actor, actor->target, MT_TROOPSHOT);  // launch a missile
}

void A_SargAttack(mobj_t *actor)
{
  if (!actor->target)
    return;
  A_FaceTarget(actor);
  if (P_CheckMeleeRange(actor))
    {
      int damage = ((P_Random(pr_sargattack)%10)+1)*4;
      P_DamageMobj(actor->target, actor, actor, damage);
    }
}

void A_HeadAttack(mobj_t *actor)
{
  if (!actor->target)
    return;
  A_FaceTarget (actor);
  if (P_CheckMeleeRange(actor))
    {
      int damage = (P_Random(pr_headattack)%6+1)*10;
      P_DamageMobj(actor->target, actor, actor, damage);
      return;
    }
  P_SpawnMissile(actor, actor->target, MT_HEADSHOT);  // launch a missile
}

void A_CyberAttack(mobj_t *actor)
{
  if (!actor->target)
    return;
  A_FaceTarget(actor);
  P_SpawnMissile(actor, actor->target, MT_ROCKET);
}

void A_BruisAttack(mobj_t *actor)
{
  if (!actor->target)
    return;
  if (P_CheckMeleeRange(actor))
    {
      int damage;
      S_StartSound(actor, sfx_claw);
      damage = (P_Random(pr_bruisattack)%8+1)*10;
      P_DamageMobj(actor->target, actor, actor, damage);
      return;
    }
  P_SpawnMissile(actor, actor->target, MT_BRUISERSHOT);  // launch a missile
}

//
// A_SkelMissile
//

void A_SkelMissile(mobj_t *actor)
{
  mobj_t *mo;

  if (!actor->target)
    return;

  A_FaceTarget (actor);
  actor->z += 16*FRACUNIT;      // so missile spawns higher
  mo = P_SpawnMissile (actor, actor->target, MT_TRACER);
  actor->z -= 16*FRACUNIT;      // back to normal

  mo->x += mo->momx;
  mo->y += mo->momy;
  mo->tracer = actor->target;
}

int     TRACEANGLE = 0xc000000;

void A_Tracer(mobj_t *actor)
{
  angle_t       exact;
  fixed_t       dist;
  fixed_t       slope;
  mobj_t        *dest;
  mobj_t        *th;

  // killough 1/18/98: this is why some missiles do not have smoke
  // and some do. Also, internal demos start at random gametics, thus
  // the bug in which revenants cause internal demos to go out of sync.
  //
  // killough 3/6/98: fix revenant internal demo bug by subtracting
  // levelstarttic from gametic:

  if ((gametic-levelstarttic) & 3)
    return;

  // spawn a puff of smoke behind the rocket
  P_SpawnPuff(actor->x, actor->y, actor->z);

  th = P_SpawnMobj (actor->x-actor->momx,
                    actor->y-actor->momy,
                    actor->z, MT_SMOKE);

  th->momz = FRACUNIT;
  th->tics -= P_Random(pr_tracer) & 3;
  if (th->tics < 1)
    th->tics = 1;

  // adjust direction
  dest = actor->tracer;

  if (!dest || dest->health <= 0)
    return;

  // change angle
  exact = R_PointToAngle2(actor->x, actor->y, dest->x, dest->y);

  if (exact != actor->angle)
    if (exact - actor->angle > 0x80000000)
      {
        actor->angle -= TRACEANGLE;
        if (exact - actor->angle < 0x80000000)
          actor->angle = exact;
      }
    else
      {
        actor->angle += TRACEANGLE;
        if (exact - actor->angle > 0x80000000)
          actor->angle = exact;
      }

  exact = actor->angle>>ANGLETOFINESHIFT;
  actor->momx = FixedMul(actor->info->speed, finecosine[exact]);
  actor->momy = FixedMul(actor->info->speed, finesine[exact]);

  // change slope
  dist = P_AproxDistance(dest->x - actor->x, dest->y - actor->y);

  dist = dist / actor->info->speed;

  if (dist < 1)
    dist = 1;

  slope = (dest->z+40*FRACUNIT - actor->z) / dist;

  if (slope < actor->momz)
    actor->momz -= FRACUNIT/8;
  else
    actor->momz += FRACUNIT/8;
}

void A_SkelWhoosh(mobj_t *actor)
{
  if (!actor->target)
    return;
  A_FaceTarget(actor);
  S_StartSound(actor,sfx_skeswg);
}

void A_SkelFist(mobj_t *actor)
{
  if (!actor->target)
    return;
  A_FaceTarget(actor);
  if (P_CheckMeleeRange(actor))
    {
      int damage = ((P_Random(pr_skelfist)%10)+1)*6;
      S_StartSound(actor, sfx_skepch);
      P_DamageMobj(actor->target, actor, actor, damage);
    }
}

//
// PIT_VileCheck
// Detect a corpse that could be raised.
//

mobj_t* corpsehit;
mobj_t* vileobj;
fixed_t viletryx;
fixed_t viletryy;

boolean PIT_VileCheck(mobj_t *thing)
{
  int     maxdist;
  boolean check;

  if (!(thing->flags & MF_CORPSE) )
    return true;        // not a monster

  if (thing->tics != -1)
    return true;        // not lying still yet

  if (thing->info->raisestate == S_NULL)
    return true;        // monster doesn't have a raise state

  maxdist = thing->info->radius + mobjinfo[MT_VILE].radius;

  if (abs(thing->x-viletryx) > maxdist || abs(thing->y-viletryy) > maxdist)
    return true;                // not actually touching

// Check to see if the radius and height are zero. If they are      // phares
// then this is a crushed monster that has been turned into a       //   |
// gib. One of the options may be to ignore this guy.               //   V

// Option 1: the original, buggy method, -> ghost (compatibility)
// Option 2: ressurect the monster, but not as a ghost
// Option 3: ignore the gib

//    if (Option3)                                                  //   ^
//        if ((thing->height == 0) && (thing->radius == 0))         //   |
//            return true;                                          // phares

    corpsehit = thing;
    corpsehit->momx = corpsehit->momy = 0;
    if (compatibility)                                              // phares
      {                                                             //   |
        corpsehit->height <<= 2;                                    //   V
        check = P_CheckPosition(corpsehit,corpsehit->x,corpsehit->y);
        corpsehit->height >>= 2;
      }
    else
      {
        int height,radius;

        height = corpsehit->height; // save temporarily
        radius = corpsehit->radius; // save temporarily
        corpsehit->height = corpsehit->info->height;
        corpsehit->radius = corpsehit->info->radius;
        corpsehit->flags |= MF_SOLID;
        check = P_CheckPosition(corpsehit,corpsehit->x,corpsehit->y);
        corpsehit->height = height; // restore
        corpsehit->radius = radius; // restore                      //   ^
        corpsehit->flags &= ~MF_SOLID;
      }                                                             //   |
                                                                    // phares
    if (!check)
      return true;              // doesn't fit here
    return false;               // got one, so stop checking
}

//
// A_VileChase
// Check for ressurecting a body
//

void A_VileChase(mobj_t* actor)
{
  int xl, xh;
  int yl, yh;
  int bx, by;
  mobjinfo_t *info;
  mobj_t *temp;

  if (actor->movedir != DI_NODIR)
    {
      // check for corpses to raise
      viletryx =
        actor->x + actor->info->speed*xspeed[actor->movedir];
      viletryy =
        actor->y + actor->info->speed*yspeed[actor->movedir];

      xl = (viletryx - bmaporgx - MAXRADIUS*2)>>MAPBLOCKSHIFT;
      xh = (viletryx - bmaporgx + MAXRADIUS*2)>>MAPBLOCKSHIFT;
      yl = (viletryy - bmaporgy - MAXRADIUS*2)>>MAPBLOCKSHIFT;
      yh = (viletryy - bmaporgy + MAXRADIUS*2)>>MAPBLOCKSHIFT;

      vileobj = actor;
      for (bx=xl ; bx<=xh ; bx++)
        {
          for (by=yl ; by<=yh ; by++)
            {
              // Call PIT_VileCheck to check
              // whether object is a corpse
              // that canbe raised.
              if (!P_BlockThingsIterator(bx,by,PIT_VileCheck))
                {
                  // got one!
                  temp = actor->target;
                  actor->target = corpsehit;
                  A_FaceTarget(actor);
                  actor->target = temp;

                  P_SetMobjState(actor, S_VILE_HEAL1);
                  S_StartSound(corpsehit, sfx_slop);
                  info = corpsehit->info;

                  P_SetMobjState(corpsehit,info->raisestate);

                  if (compatibility)                                // phares
                    corpsehit->height <<= 2;                        //   |
                  else                                              //   V
                    {
                      corpsehit->height = info->height; // fix Ghost bug
                      corpsehit->radius = info->radius; // fix Ghost bug
                    }                                               // phares
                  corpsehit->flags = info->flags;
                  corpsehit->health = info->spawnhealth;
                  corpsehit->target = NULL;
                  return;
                }
            }
        }
    }
  A_Chase(actor);  // Return to normal attack.
}

//
// A_VileStart
//

void A_VileStart(mobj_t *actor)
{
  S_StartSound(actor, sfx_vilatk);
}

//
// A_Fire
// Keep fire in front of player unless out of sight
//

void A_Fire(mobj_t *actor);

void A_StartFire(mobj_t *actor)
{
  S_StartSound(actor,sfx_flamst);
  A_Fire(actor);
}

void A_FireCrackle(mobj_t* actor)
{
  S_StartSound(actor,sfx_flame);
  A_Fire(actor);
}

void A_Fire(mobj_t *actor)
{
  unsigned an;
  mobj_t *dest = actor->tracer;

  if (!dest)
    return;

  // don't move it if the vile lost sight
  if (!P_CheckSight(actor->target, dest) )
    return;

  an = dest->angle >> ANGLETOFINESHIFT;

  P_UnsetThingPosition(actor);
  actor->x = dest->x + FixedMul(24*FRACUNIT, finecosine[an]);
  actor->y = dest->y + FixedMul(24*FRACUNIT, finesine[an]);
  actor->z = dest->z;
  P_SetThingPosition(actor);
}

//
// A_VileTarget
// Spawn the hellfire
//

void A_VileTarget(mobj_t *actor)
{
  mobj_t *fog;

  if (!actor->target)
    return;

  A_FaceTarget(actor);

  fog = P_SpawnMobj(actor->target->x,
                    actor->target->x, // huh? this is correct!
                    actor->target->z,MT_FIRE);

  actor->tracer = fog;
  fog->target = actor;
  fog->tracer = actor->target;
  A_Fire(fog);
}

//
// A_VileAttack
//

void A_VileAttack(mobj_t *actor)
{
  mobj_t *fire;
  int    an;

  if (!actor->target)
    return;

  A_FaceTarget(actor);

  if (!P_CheckSight(actor, actor->target))
    return;

  S_StartSound(actor, sfx_barexp);
  P_DamageMobj(actor->target, actor, actor, 20);
  actor->target->momz = 1000*FRACUNIT/actor->target->info->mass;

  an = actor->angle >> ANGLETOFINESHIFT;

  fire = actor->tracer;

  if (!fire)
    return;

  // move the fire between the vile and the player
  fire->x = actor->target->x - FixedMul (24*FRACUNIT, finecosine[an]);
  fire->y = actor->target->y - FixedMul (24*FRACUNIT, finesine[an]);
  P_RadiusAttack(fire, actor, 70);
}

//
// Mancubus attack,
// firing three missiles (bruisers)
// in three different directions?
// Doesn't look like it.
//

#define FATSPREAD       (ANG90/8)

void A_FatRaise(mobj_t *actor)
{
  A_FaceTarget(actor);
  S_StartSound(actor, sfx_manatk);
}

void A_FatAttack1(mobj_t *actor)
{
  mobj_t *mo;
  int    an;

  A_FaceTarget(actor);

  // Change direction  to ...
  actor->angle += FATSPREAD;

  P_SpawnMissile(actor, actor->target, MT_FATSHOT);

  mo = P_SpawnMissile (actor, actor->target, MT_FATSHOT);
  mo->angle += FATSPREAD;
  an = mo->angle >> ANGLETOFINESHIFT;
  mo->momx = FixedMul(mo->info->speed, finecosine[an]);
  mo->momy = FixedMul(mo->info->speed, finesine[an]);
}

void A_FatAttack2(mobj_t *actor)
{
  mobj_t *mo;
  int    an;

  A_FaceTarget(actor);
  // Now here choose opposite deviation.
  actor->angle -= FATSPREAD;
  P_SpawnMissile(actor, actor->target, MT_FATSHOT);

  mo = P_SpawnMissile(actor, actor->target, MT_FATSHOT);
  mo->angle -= FATSPREAD*2;
  an = mo->angle >> ANGLETOFINESHIFT;
  mo->momx = FixedMul(mo->info->speed, finecosine[an]);
  mo->momy = FixedMul(mo->info->speed, finesine[an]);
}

void A_FatAttack3(mobj_t *actor)
{
  mobj_t *mo;
  int    an;

  A_FaceTarget(actor);

  mo = P_SpawnMissile(actor, actor->target, MT_FATSHOT);
  mo->angle -= FATSPREAD/2;
  an = mo->angle >> ANGLETOFINESHIFT;
  mo->momx = FixedMul(mo->info->speed, finecosine[an]);
  mo->momy = FixedMul(mo->info->speed, finesine[an]);

  mo = P_SpawnMissile(actor, actor->target, MT_FATSHOT);
  mo->angle += FATSPREAD/2;
  an = mo->angle >> ANGLETOFINESHIFT;
  mo->momx = FixedMul(mo->info->speed, finecosine[an]);
  mo->momy = FixedMul(mo->info->speed, finesine[an]);
}


//
// SkullAttack
// Fly at the player like a missile.
//
#define SKULLSPEED              (20*FRACUNIT)

void A_SkullAttack(mobj_t *actor)
{
  mobj_t  *dest;
  angle_t an;
  int     dist;

  if (!actor->target)
    return;

  dest = actor->target;
  actor->flags |= MF_SKULLFLY;

  S_StartSound(actor, actor->info->attacksound);
  A_FaceTarget(actor);
  an = actor->angle >> ANGLETOFINESHIFT;
  actor->momx = FixedMul(SKULLSPEED, finecosine[an]);
  actor->momy = FixedMul(SKULLSPEED, finesine[an]);
  dist = P_AproxDistance(dest->x - actor->x, dest->y - actor->y);
  dist = dist / SKULLSPEED;

  if (dist < 1)
    dist = 1;
  actor->momz = (dest->z+(dest->height>>1) - actor->z) / dist;
}

//
// A_PainShootSkull
// Spawn a lost soul and launch it at the target
//

void A_PainShootSkull(mobj_t *actor, angle_t angle)
{
  fixed_t       x,y,z;
  mobj_t        *newmobj;
  angle_t       an;
  int           prestep;

// The original code checked for 20 skulls on the level,            // phares
// and wouldn't spit another one if there were. If not in           // phares
// compatibility mode, we remove the limit.                         // phares
                                                                    // phares
  if (compatibility)  // original or bug fix extension              // phares
    {
      // count total number of skulls currently on the level
      int count = 0;
      thinker_t *currentthinker;
      for (currentthinker = thinkercap.next;
           currentthinker != &thinkercap;
           currentthinker = currentthinker->next)
        if ((currentthinker->function.acp1 == (actionf_p1) P_MobjThinker)
            && ((mobj_t *)currentthinker)->type == MT_SKULL)
          count++;
      if (count > 20)                                               // phares
        return;                                                     // phares
    }

  // okay, there's room for another one

  an = angle >> ANGLETOFINESHIFT;

  prestep = 4*FRACUNIT + 3*(actor->info->radius + mobjinfo[MT_SKULL].radius)/2;

  x = actor->x + FixedMul(prestep, finecosine[an]);
  y = actor->y + FixedMul(prestep, finesine[an]);
  z = actor->z + 8*FRACUNIT;

  if (compatibility)                                              // phares
    newmobj = P_SpawnMobj(x, y, z, MT_SKULL);                     //   |
  else                                                            //   V
    {
      // Check whether the Lost Soul is being fired through a 1-sided
      // wall or an impassible line, or a "monsters can't cross" line.
      // If it is, then we don't allow the spawn. This is a bug fix, but
      // it should be considered an enhancement, since it may disturb
      // existing demos, so don't do it in compatibility mode.

      if (Check_Sides(actor,x,y))
        return;

      newmobj = P_SpawnMobj(x, y, z, MT_SKULL);

      // Check to see if the new Lost Soul's z value is above the
      // ceiling of its new sector, or below the floor. If so, kill it.

      if ((newmobj->z >
           (newmobj->subsector->sector->ceilingheight - newmobj->height)) ||
          (newmobj->z < newmobj->subsector->sector->floorheight))
        {
          // kill it immediately
          P_DamageMobj(newmobj,actor,actor,10000);
          return;                                                 //   ^
        }                                                         //   |
     }                                                            // phares

  // Check for movements.
  // killough 3/15/98: don't jump over dropoffs:

  if (!P_TryMove(newmobj, newmobj->x, newmobj->y, false))
    {
      // kill it immediately
      P_DamageMobj(newmobj, actor, actor, 10000);
      return;
    }

  newmobj->target = actor->target;
  A_SkullAttack(newmobj);
}

//
// A_PainAttack
// Spawn a lost soul and launch it at the target
//

void A_PainAttack(mobj_t *actor)
{
  if (!actor->target)
    return;
  A_FaceTarget(actor);
  A_PainShootSkull(actor, actor->angle);
}

void A_PainDie(mobj_t *actor)
{
  A_Fall(actor);
  A_PainShootSkull(actor, actor->angle+ANG90);
  A_PainShootSkull(actor, actor->angle+ANG180);
  A_PainShootSkull(actor, actor->angle+ANG270);
}

void A_Scream(mobj_t *actor)
{
  int sound;

  switch (actor->info->deathsound)
    {
    case 0:
      return;

    case sfx_podth1:
    case sfx_podth2:
    case sfx_podth3:
      sound = sfx_podth1 + P_Random(pr_scream)%3;
      break;

    case sfx_bgdth1:
    case sfx_bgdth2:
      sound = sfx_bgdth1 + P_Random(pr_scream)%2;
      break;

    default:
      sound = actor->info->deathsound;
      break;
    }

  // Check for bosses.
  if (actor->type==MT_SPIDER || actor->type == MT_CYBORG)
    S_StartSound(NULL, sound); // full volume
  else
    S_StartSound(actor, sound);
}

void A_XScream(mobj_t *actor)
{
  S_StartSound(actor, sfx_slop);
}

void A_Pain(mobj_t *actor)
{
  if (actor->info->painsound)
    S_StartSound(actor, actor->info->painsound);
}

void A_Fall(mobj_t *actor)
{
  // actor is on ground, it can be walked over
  actor->flags &= ~MF_SOLID;
}

//
// A_Explode
//
void A_Explode(mobj_t *thingy)
{
  P_RadiusAttack( thingy, thingy->target, 128 );
}

//
// A_BossDeath
// Possibly trigger special effects
// if on first boss level
//

void A_BossDeath(mobj_t *mo)
{
  thinker_t *th;
  line_t    junk;
  int       i;

  if (gamemode == commercial)
    {
      if (gamemap != 7)
        return;

      if ((mo->type != MT_FATSO)
          && (mo->type != MT_BABY))
        return;
    }
  else
    {
      switch(gameepisode)
        {
        case 1:
          if (gamemap != 8)
            return;

          if (mo->type != MT_BRUISER)
            return;
          break;

        case 2:
          if (gamemap != 8)
            return;

          if (mo->type != MT_CYBORG)
            return;
          break;

        case 3:
          if (gamemap != 8)
            return;

          if (mo->type != MT_SPIDER)
            return;

          break;

        case 4:
          switch(gamemap)
            {
            case 6:
              if (mo->type != MT_CYBORG)
                return;
              break;

            case 8:
              if (mo->type != MT_SPIDER)
                return;
              break;

            default:
              return;
              break;
            }
          break;

        default:
          if (gamemap != 8)
            return;
          break;
        }

    }

  // make sure there is a player alive for victory
  for (i=0; i<MAXPLAYERS; i++)
    if (playeringame[i] && players[i].health > 0)
      break;

  if (i==MAXPLAYERS)
    return;     // no one left alive, so do not end game

    // scan the remaining thinkers to see
    // if all bosses are dead
  for (th = thinkercap.next ; th != &thinkercap ; th=th->next)
    if (th->function.acp1 == (actionf_p1)P_MobjThinker)
      {
        mobj_t *mo2 = (mobj_t *) th;
        if (mo2 != mo && mo2->type == mo->type && mo2->health > 0)
          return;         // other boss not dead
      }

  // victory!
  if ( gamemode == commercial)
    {
      if (gamemap == 7)
        {
          if (mo->type == MT_FATSO)
            {
              junk.tag = 666;
              EV_DoFloor(&junk,lowerFloorToLowest);
              return;
            }

          if (mo->type == MT_BABY)
            {
              junk.tag = 667;
              EV_DoFloor(&junk,raiseToTexture);
              return;
            }
        }
    }
  else
    {
      switch(gameepisode)
        {
        case 1:
          junk.tag = 666;
          EV_DoFloor(&junk, lowerFloorToLowest);
          return;
          break;

        case 4:
          switch(gamemap)
            {
            case 6:
              junk.tag = 666;
              EV_DoDoor(&junk, blazeOpen);
              return;
              break;

            case 8:
              junk.tag = 666;
              EV_DoFloor(&junk, lowerFloorToLowest);
              return;
              break;
            }
        }
    }
  G_ExitLevel();
}


void A_Hoof (mobj_t* mo)
{
    S_StartSound(mo, sfx_hoof);
    A_Chase(mo);
}

void A_Metal(mobj_t *mo)
{
  S_StartSound(mo, sfx_metal);
  A_Chase(mo);
}

void A_BabyMetal(mobj_t *mo)
{
  S_StartSound(mo, sfx_bspwlk);
  A_Chase(mo);
}

void A_OpenShotgun2(player_t *player, pspdef_t *psp)
{
  S_StartSound(player->mo, sfx_dbopn);
}

void A_LoadShotgun2(player_t *player, pspdef_t *psp)
{
  S_StartSound(player->mo, sfx_dbload);
}

void A_ReFire(player_t *player, pspdef_t *psp);

void A_CloseShotgun2(player_t *player, pspdef_t *psp)
{
  S_StartSound(player->mo, sfx_dbcls);
  A_ReFire(player,psp);
}

// killough 2/7/98: Remove limit on icon landings:
mobj_t **braintargets;
int    numbraintargets_alloc;
int    numbraintargets;

struct brain_s brain;   // killough 3/26/98: global state of boss brain

// killough 3/26/98: initialize icon landings at level startup,
// rather than at boss wakeup, to prevent savegame-related crashes

void P_SpawnBrainTargets(void)  // killough 3/26/98: renamed old function
{
  thinker_t *thinker;

  // find all the target spots
  numbraintargets = 0;
  brain.targeton = 0;
  brain.easy = 0;           // killough 3/26/98: always init easy to 0

  for (thinker = thinkercap.next ;
       thinker != &thinkercap ;
       thinker = thinker->next)
    if (thinker->function.acp1 == (actionf_p1)P_MobjThinker)
      {
        mobj_t *m = (mobj_t *) thinker;

        if (m->type == MT_BOSSTARGET )
          {   // killough 2/7/98: remove limit on icon landings:
            if (numbraintargets >= numbraintargets_alloc)
              braintargets = realloc(braintargets,
                      (numbraintargets_alloc = numbraintargets_alloc ?
                       numbraintargets_alloc*2 : 32) *sizeof *braintargets);
            braintargets[numbraintargets++] = m;
          }
      }
}

void A_BrainAwake(mobj_t *mo)
{
  S_StartSound(NULL,sfx_bossit); // killough 3/26/98: only generates sound now
}

void A_BrainPain(mobj_t *mo)
{
  S_StartSound(NULL,sfx_bospn);
}

void A_BrainScream(mobj_t *mo)
{
  int x;
  for (x=mo->x - 196*FRACUNIT ; x< mo->x + 320*FRACUNIT ; x+= FRACUNIT*8)
    {
      int y = mo->y - 320*FRACUNIT;
      int z = 128 + P_Random(pr_brainscream)*2*FRACUNIT;
      mobj_t *th = P_SpawnMobj (x,y,z, MT_ROCKET);
      th->momz = P_Random(pr_brainscream)*512;
      P_SetMobjState(th, S_BRAINEXPLODE1);
      th->tics -= P_Random(pr_brainscream)&7;
      if (th->tics < 1)
        th->tics = 1;
    }
  S_StartSound(NULL,sfx_bosdth);
}

void A_BrainExplode(mobj_t *mo)
{  // killough 5/5/98: remove dependence on order of evaluation:
  int t = P_Random(pr_brainexp);
  int x = mo->x + (t - P_Random(pr_brainexp))*2048;
  int y = mo->y;
  int z = 128 + P_Random(pr_brainexp)*2*FRACUNIT;
  mobj_t *th = P_SpawnMobj(x,y,z, MT_ROCKET);
  th->momz = P_Random(pr_brainexp)*512;
  P_SetMobjState(th, S_BRAINEXPLODE1);
  th->tics -= P_Random(pr_brainexp)&7;
  if (th->tics < 1)
    th->tics = 1;
}

void A_BrainDie(mobj_t *mo)
{
  G_ExitLevel();
}

void A_BrainSpit(mobj_t *mo)
{
  mobj_t *targ, *newmobj;

  if (!numbraintargets)     // killough 4/1/98: ignore if no targets
    return;

  brain.easy ^= 1;          // killough 3/26/98: use brain struct
  if (gameskill <= sk_easy && !brain.easy)
    return;

  // shoot a cube at current target
  targ = braintargets[brain.targeton++]; // killough 3/26/98:
  brain.targeton %= numbraintargets;     // Use brain struct for targets

  // spawn brain missile
  newmobj = P_SpawnMissile(mo, targ, MT_SPAWNSHOT);
  newmobj->target = targ;
  newmobj->reactiontime = ((targ->y-mo->y)/newmobj->momy)/newmobj->state->tics;

  S_StartSound(NULL, sfx_bospit);
}

void A_SpawnFly(mobj_t *mo);

// travelling cube sound
void A_SpawnSound(mobj_t *mo)
{
  S_StartSound(mo,sfx_boscub);
  A_SpawnFly(mo);
}

void A_SpawnFly(mobj_t *mo)
{
  mobj_t *newmobj;
  mobj_t *fog;
  mobj_t *targ;
  int    r;
  mobjtype_t type;

  if (--mo->reactiontime)
    return;     // still flying

  targ = mo->target;

  // First spawn teleport fog.
  fog = P_SpawnMobj(targ->x, targ->y, targ->z, MT_SPAWNFIRE);
  S_StartSound(fog, sfx_telept);

  // Randomly select monster to spawn.
  r = P_Random(pr_spawnfly);

  // Probability distribution (kind of :), decreasing likelihood.
  if ( r<50 )
    type = MT_TROOP;
  else if (r<90)
    type = MT_SERGEANT;
  else if (r<120)
    type = MT_SHADOWS;
  else if (r<130)
    type = MT_PAIN;
  else if (r<160)
    type = MT_HEAD;
  else if (r<162)
    type = MT_VILE;
  else if (r<172)
    type = MT_UNDEAD;
  else if (r<192)
    type = MT_BABY;
  else if (r<222)
    type = MT_FATSO;
  else if (r<246)
    type = MT_KNIGHT;
  else
    type = MT_BRUISER;

  newmobj = P_SpawnMobj(targ->x, targ->y, targ->z, type);
  if (P_LookForPlayers(newmobj, true) )
    P_SetMobjState(newmobj, newmobj->info->seestate);

    // telefrag anything in this spot
  P_TeleportMove(newmobj, newmobj->x, newmobj->y);

  // remove self (i.e., cube).
  P_RemoveMobj(mo);
}

void A_PlayerScream(mobj_t *mo)
{
  int sound = sfx_pldeth;  // Default death sound.
  if (gamemode == commercial && mo->health < -50)
    sound = sfx_pdiehi;   // IF THE PLAYER DIES LESS THAN -50% WITHOUT GIBBING
  S_StartSound(mo, sound);
}

//----------------------------------------------------------------------------
//
// $Log: p_enemy.c,v $
// Revision 1.23  1998/08/13  15:27:26  jim
// Doorjamb fix
//
// Revision 1.22  1998/05/12  12:47:10  phares
// Removed OVER_UNDER code
//
// Revision 1.21  1998/05/07  00:50:55  killough
// beautification, remove dependence on evaluation order
//
// Revision 1.20  1998/05/03  22:28:02  killough
// beautification, move declarations and includes around
//
// Revision 1.19  1998/04/01  12:58:44  killough
// Disable boss brain if no targets
//
// Revision 1.18  1998/03/28  17:57:05  killough
// Fix boss spawn savegame bug
//
// Revision 1.17  1998/03/23  15:18:03  phares
// Repaired AV ghosts stuck together bug
//
// Revision 1.16  1998/03/16  12:33:12  killough
// Use new P_TryMove()
//
// Revision 1.15  1998/03/09  07:17:58  killough
// Fix revenant tracer bug
//
// Revision 1.14  1998/03/02  11:40:52  killough
// Use separate monsters_remember flag instead of bitmask
//
// Revision 1.13  1998/02/24  08:46:12  phares
// Pushers, recoil, new friction, and over/under work
//
// Revision 1.12  1998/02/23  04:43:44  killough
// Add revenant p_atracer, optioned monster ai_vengence
//
// Revision 1.11  1998/02/17  06:04:55  killough
// Change RNG calling sequences
// Fix minor icon landing bug
// Use lastenemy to make monsters remember former targets, and fix player look
//
// Revision 1.10  1998/02/09  03:05:22  killough
// Remove icon landing limit
//
// Revision 1.9  1998/02/05  12:15:39  phares
// tighten lost soul wall fix to compatibility
//
// Revision 1.8  1998/02/02  13:42:54  killough
// Relax lost soul wall fix to demo_compatibility
//
// Revision 1.7  1998/01/28  13:21:01  phares
// corrected Option3 in AV bug
//
// Revision 1.6  1998/01/28  12:22:17  phares
// AV bug fix and Lost Soul trajectory bug fix
//
// Revision 1.5  1998/01/26  19:24:00  phares
// First rev with no ^Ms
//
// Revision 1.4  1998/01/23  14:51:51  phares
// No content change. Put ^Ms back.
//
// Revision 1.3  1998/01/23  14:42:14  phares
// No content change. Removed ^Ms for experimental checkin.
//
// Revision 1.2  1998/01/19  14:45:01  rand
// Temporary line for checking checkins
//
// Revision 1.1.1.1  1998/01/19  14:02:59  rand
// Lee's Jan 19 sources
//
//----------------------------------------------------------------------------
