/* mechsim.h - mechanical study runtime: actuators and transmissions, sampled controllers, sensors with reproducible
 * measurement models, event scheduling, recorded histories, joint load envelopes, checkpoint and rollback.
 *
 * Time
 *   The multibody state advances with steps no longer than max_step that land exactly on every controller sample,
 *   sensor sample and record instant (instants are k * period computed from integer counters, so they never drift).
 *   Controllers run at their own period with an output delay of whole samples and hold their output between samples.
 *   Sensors sample at their own period; latency is a whole number of sensor samples. The AI configures all of this;
 *   none of it runs in the loop through MCP.
 *
 * Actuators act on revolute or prismatic joints. Joint-side quantities: q, v, effort tau (N m or N).
 *   effort          command = joint effort; clamped to effort_limit; with velocity_limit the driving effort follows the
 *                   linear envelope effort_limit * (1 - |v| / velocity_limit) (braking effort up to effort_limit)
 *   position_servo  command = joint position; tau = kp (command - q) - kd v, within the same envelope (the servo's
 *                   internal loop is treated as continuous)
 *   velocity_servo  command = joint velocity; tau = kv (command - v), within the envelope
 *   dc_motor        command = voltage. Motor speed w = N v (N = gear_ratio, motor rad per joint unit);
 *                   V = clamp(command, +-voltage_limit); i = clamp((V - ke w) / R, +-current_limit) (inductance neglected);
 *                   tau_m = kt i - b w - c tanh(w / w_reg); joint effort = eta N tau_m when the motor drives (tau_m w >= 0)
 *                   and N tau_m / eta when it is back-driven; rotor inertia is reflected as armature N^2 J_rotor.
 *   Losses (W) are exported for the thermal side: copper i^2 R, motor friction, gearbox (1 - eta) share. They are
 *   computed here once and must not be recomputed elsewhere.
 * Sensors return a measured value next to the ground truth: measured = quantize(filter(truth + bias + noise)), delayed by
 *   the latency. Noise is Gaussian from a per-sensor xoshiro256** generator seeded explicitly; its state is part of the
 *   checkpoint, so a rolled-back and retried step draws the same samples again. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../core/json.h"
#include "contact.h"
#include "mechdiag.h"
#include "multibody.h"

enum { MS_MAX_DELAY = 64, MS_MAX_LATENCY = 64, MS_MAX_WAYPOINTS = 32 };

typedef enum { ACT_EFFORT = 0, ACT_POSITION_SERVO, ACT_VELOCITY_SERVO, ACT_DC_MOTOR, ACT_TYPES } ActuatorType;
const char *actuator_type_name(ActuatorType t);
int actuator_type_from_name(const char *s);

typedef struct ActuatorDef {
    char name[MB_NAME];
    ActuatorType type;
    int joint;                /* model joint index */
    double effort_limit;      /* joint side (0 = unlimited; required for servos) */
    double velocity_limit;    /* joint side (0 = no torque-speed envelope) */
    double rated_effort;      /* gearbox or product rating at the joint; exceeding it is flagged, not clamped (0 = none) */
    double kp, kd, kv;        /* servo gains (joint side) */
    /* dc motor */
    double gear_ratio;        /* N (>0): motor displacement per joint displacement */
    double efficiency;        /* 0 < eta <= 1 */
    double rotor_inertia;     /* kg m^2 at the motor shaft */
    double resistance;        /* ohm */
    double torque_constant;   /* N m/A */
    double back_emf_constant; /* V s/rad */
    double voltage_limit;     /* V */
    double current_limit;     /* A (0 = none) */
    double motor_viscous;     /* N m s/rad at the motor shaft */
    double motor_coulomb;     /* N m at the motor shaft */
    double motor_coulomb_vreg; /* rad/s */
    double initial_command;
} ActuatorDef;

typedef enum { TRAJ_CONSTANT = 0, TRAJ_STEP, TRAJ_RAMP, TRAJ_SINE, TRAJ_TRAPEZOID, TRAJ_MIN_JERK, TRAJ_WAYPOINTS, TRAJ_TYPES } TrajType;
const char *trajectory_type_name(TrajType t);
int trajectory_type_from_name(const char *s);

typedef struct Trajectory {
    TrajType type;
    double t0;                 /* start time (s) */
    double value, value1;      /* constant / step, ramp and point-to-point: from value to value1 */
    double rate;               /* ramp slope */
    double amplitude, frequency, phase; /* sine: value + amplitude sin(2 pi f (t - t0) + phase) */
    double duration;           /* min_jerk: motion time */
    double vmax, amax;         /* trapezoid limits */
    int npoints;               /* waypoints: rest-to-rest minimum-jerk segments */
    double wt[MS_MAX_WAYPOINTS], wp[MS_MAX_WAYPOINTS];
} Trajectory;
/* position, velocity and acceleration of the reference at time t */
void trajectory_eval(const Trajectory *tr, double t, double *p, double *v, double *a);

typedef enum { CTRL_PID = 0, CTRL_OPEN_LOOP, CTRL_TYPES } ControllerType;
typedef enum { AW_NONE = 0, AW_CLAMP, AW_BACK_CALCULATION } AntiWindup;

typedef struct ControllerDef {
    char name[MB_NAME];
    ControllerType type;
    int actuator;            /* index into the actuator list */
    int feedback_sensor;     /* sensor index measuring the controlled variable; -1 = ideal state feedback (explicit) */
    bool control_velocity;   /* false: position loop; true: velocity loop */
    double period;           /* s */
    int delay_samples;       /* whole periods between computing and applying an output */
    double kp, ki, kd;
    double derivative_filter; /* s, first-order filter time constant on the derivative (0 = none) */
    double kff_velocity, kff_acceleration; /* feedforward gains on the reference derivatives */
    double output_min, output_max;         /* saturation in command units */
    AntiWindup antiwindup;
    double tracking_time;    /* back-calculation time constant (s) */
    Trajectory reference;
} ControllerDef;

typedef enum { SENS_JOINT_POSITION = 0, SENS_JOINT_VELOCITY, SENS_JOINT_EFFORT, SENS_MOTOR_CURRENT, SENS_FORCE_TORQUE, SENS_IMU_ACCEL, SENS_IMU_GYRO, SENS_TYPES } SensorType;
const char *sensor_type_name(SensorType t);
int sensor_type_from_name(const char *s);
int sensor_dim(SensorType t); /* number of channels (1, 3 or 6) */

typedef struct SensorDef {
    char name[MB_NAME];
    SensorType type;
    int joint;               /* joint sensors and force_torque */
    int actuator;            /* joint_effort, motor_current */
    int body;                /* imu */
    MPose site;              /* imu frame in body coordinates */
    double period;           /* s */
    int latency_samples;
    double noise_std, bias;  /* per channel, in SI units of the quantity */
    double resolution;       /* quantisation step (0 = none) */
    double filter_tau;       /* first-order low-pass time constant (0 = none) */
    uint64_t seed;
} SensorDef;

typedef struct StudySettings {
    double end_time, max_step;      /* s */
    double record_period;           /* s (0 = every controller sample, or max_step) */
    int max_records;                /* history rows kept (default 200000) */
    /* contact between bodies: switches the integration to first-order time stepping (Moreau-Jean) */
    bool contact;
    double contact_margin;          /* m (0 = 1 mm) */
    int contact_max_iterations;     /* 0 = 500 */
    double contact_tolerance;       /* 0 = 1e-10 */
    double contact_restitution_speed; /* m/s (0 = 1 mm/s) */
    /* flexible bodies start in static equilibrium under the initial loads with the joints held (default), or undeformed */
    bool flexible_undeformed;
} StudySettings;

typedef struct MechSim MechSim;

/* copies the model definition and the components; motor rotor inertia is reflected into the joint armature of the copy and
 * the model is compiled and owned by the runtime (NULL with diagnostics on failure) */
MechSim *mechsim_new(const MbModelDef *def, const MbOptions *opt, const ActuatorDef *act, int nact, const ControllerDef *ctl, int nctl,
                     const SensorDef *sen, int nsen, const StudySettings *st, MechDiag *diag);
/* contact shapes (copied; hull vertex arrays must outlive the runtime); call before mechsim_init */
bool mechsim_set_contact_shapes(MechSim *ms, const ContactShape *shapes, int n);
const MbModel *mechsim_model(const MechSim *ms);
void mechsim_free(MechSim *ms);
/* validates components against the model (missing inputs, unit and range problems) and assembles the initial state */
bool mechsim_init(MechSim *ms, MechDiag *diag);
/* advances to t_target (or end_time); false on failure with err */
bool mechsim_run_until(MechSim *ms, double t_target, char *err, size_t errlen);
double mechsim_time(const MechSim *ms);
/* longest step the runtime takes: max_step, or less when flexible bodies need it (0.5 / highest elastic angular frequency) */
double mechsim_step_limit(const MechSim *ms);
const MbState *mechsim_state(const MechSim *ms);
MbSim *mechsim_mb(MechSim *ms);

/* checkpoint of the complete runtime state (multibody, actuators, controllers, sensors, recorder length) */
size_t mechsim_state_size(const MechSim *ms);
bool mechsim_save_state(const MechSim *ms, uint8_t *buf, size_t cap);
bool mechsim_restore_state(MechSim *ms, const uint8_t *buf, size_t len);

/* histories: channel names and columns (row count = mechsim_rows) */
int mechsim_channels(const MechSim *ms);
const char *mechsim_channel_name(const MechSim *ms, int c);
const char *mechsim_channel_unit(const MechSim *ms, int c);
int mechsim_channel_index(const MechSim *ms, const char *name);
int mechsim_rows(const MechSim *ms);
const double *mechsim_column(const MechSim *ms, int c);

/* state snapshots for load assessment: requested instants (scheduler events) and, per joint, the states at the peak
 * force and the peak moment. A snapshot restores the complete runtime state, so the loads it yields are exactly those of
 * the run. Call before mechsim_run_until; at most 64 instants. */
bool mechsim_set_snapshot_times(MechSim *ms, const double *times, int n);
/* [{label, time_s, kind: "requested"|"peak_force"|"peak_moment", joint?, state_base64}] */
JsonValue *mechsim_snapshots_json(const MechSim *ms);
/* restores a snapshot state (recorded histories are cleared) */
bool mechsim_restore_snapshot(MechSim *ms, const uint8_t *buf, size_t len);

/* per actuator: latest applied command and electrical/mechanical state */
typedef struct ActuatorOutput {
    double command;     /* applied (after delay and hold) */
    double effort;      /* joint side */
    double voltage, current, electrical_power, copper_loss, friction_loss, gear_loss; /* dc motor */
    bool saturated;     /* effort, voltage or current at a limit */
    bool over_rating;   /* |effort| > rated_effort */
} ActuatorOutput;
bool mechsim_actuator_output(const MechSim *ms, int a, ActuatorOutput *out);

/* study summary: tracking, efforts, saturation, energy, reactions, diagnostics */
JsonValue *mechsim_summary_json(const MechSim *ms);
/* peak joint wrenches with the time and full wrench at the peak (force and moment peaks separately) */
JsonValue *mechsim_load_envelope_json(const MechSim *ms);
/* joint wrenches (6 per joint, child joint frame) and body accelerations at the current state */
bool mechsim_joint_wrenches(MechSim *ms, double *wrench6);
/* evaluates the current state (with the held actuator commands) and returns the evaluation; NULL on failure */
const MbEval *mechsim_evaluate(MechSim *ms);
/* contact rows of the last step in the snapshot format (rows with a nonzero impulse), its step and whether it was an impact
 * step; NULL without contact */
JsonValue *mechsim_last_contact_json(const MechSim *ms, double *step, bool *impulsive);
/* steps with an impact since the start of the run */
int mechsim_impulsive_steps(const MechSim *ms);
bool mechsim_write_csv(const MechSim *ms, const char *path, char *err, size_t errlen);
