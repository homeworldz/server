#include "homeworldz/physics_adapters.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace homeworldz::physics {
namespace {

namespace Layers {
constexpr JPH::ObjectLayer static_body = 0;
constexpr JPH::ObjectLayer moving_body = 1;
constexpr JPH::uint count = 2;
}
namespace BroadLayers {
const JPH::BroadPhaseLayer static_body{0};
const JPH::BroadPhaseLayer moving_body{1};
constexpr JPH::uint count = 2;
}

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadLayers::count; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == Layers::static_body ? BroadLayers::static_body : BroadLayers::moving_body;
    }
};

class ObjectBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer first, JPH::BroadPhaseLayer second) const override {
        return first == Layers::moving_body || second == BroadLayers::moving_body;
    }
};

class ObjectPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer first, JPH::ObjectLayer second) const override {
        return first == Layers::moving_body || second == Layers::moving_body;
    }
};

void initialize_jolt() {
    static std::once_flag once;
    std::call_once(once, [] {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

JPH::Vec3 vec(scene::Vector3 value) {
    return {static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z)};
}
scene::Vector3 vec(JPH::Vec3Arg value) { return {value.GetX(), value.GetY(), value.GetZ()}; }

JPH::Quat quat(const std::array<double, 4>& value) {
    const JPH::Quat result{static_cast<float>(value[0]), static_cast<float>(value[1]),
                           static_cast<float>(value[2]), static_cast<float>(value[3])};
    return result.Normalized();
}

JPH::ShapeRefC make_shape(const Shape& shape) {
    switch (shape.type) {
    case ShapeType::Sphere:
        return new JPH::SphereShape(static_cast<float>(shape.radius));
    case ShapeType::Capsule:
        return new JPH::RotatedTranslatedShape(
            JPH::Vec3::sZero(),
            JPH::Quat::sRotation(JPH::Vec3::sAxisX(), JPH::DegreesToRadians(90.0F)),
            new JPH::CapsuleShape(
                static_cast<float>(std::max(0.0, shape.height * 0.5 - shape.radius)),
                static_cast<float>(shape.radius)));
    case ShapeType::Cylinder:
        return new JPH::RotatedTranslatedShape(
            JPH::Vec3::sZero(),
            JPH::Quat::sRotation(JPH::Vec3::sAxisX(), JPH::DegreesToRadians(90.0F)),
            new JPH::CylinderShape(
                static_cast<float>(shape.height * 0.5), static_cast<float>(shape.radius)));
    case ShapeType::ConvexHull: {
        JPH::Array<JPH::Vec3> points;
        points.reserve(shape.hull_points.size());
        for (const auto& point : shape.hull_points)
            points.push_back(vec(point));
        auto result = JPH::ConvexHullShapeSettings(points, 0.0F).Create();
        if (result.HasError())
            throw std::runtime_error(
                std::string("Jolt could not create a convex hull: ") + result.GetError().c_str());
        return result.Get();
    }
    case ShapeType::Compound: {
        if (shape.compound_parts.empty())
            throw std::invalid_argument("compound shape has no parts");
        JPH::StaticCompoundShapeSettings settings;
        for (const auto& part : shape.compound_parts) {
            Shape child;
            child.type = part.type;
            child.half_extents = part.half_extents;
            child.radius = part.radius;
            child.height = part.height;
            child.hull_points = part.hull_points;
            settings.AddShape(
                vec(part.local_position), quat(part.local_rotation), make_shape(child));
        }
        auto result = settings.Create();
        if (result.HasError())
            throw std::runtime_error(
                std::string("Jolt could not create a compound: ") + result.GetError().c_str());
        return result.Get();
    }
    case ShapeType::Box:
    default:
        return new JPH::BoxShape(vec(shape.half_extents));
    }
}

JPH::ShapeRefC make_character_shape(double radius, double height) {
    const Shape capsule{ShapeType::Capsule, {}, radius, height};
    return new JPH::RotatedTranslatedShape(
        {0, 0, static_cast<float>(height * 0.5)}, JPH::Quat::sIdentity(), make_shape(capsule));
}

JPH::EMotionType motion(MotionType value) {
    if (value == MotionType::Dynamic) return JPH::EMotionType::Dynamic;
    if (value == MotionType::Kinematic) return JPH::EMotionType::Kinematic;
    return JPH::EMotionType::Static;
}

struct JoltBody {
    JPH::BodyID native;
    scene::EntityId entity{};
};

struct JoltCharacter {
    JPH::Ref<JPH::CharacterVirtual> character;
    scene::EntityId entity{};
    double height{1.8};
    double step_height{0.4};
    bool flying{};
};

class JoltWorld final : public World {
public:
    JoltWorld()
        : allocator_(16 * 1024 * 1024),
          jobs_(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                std::max(1u, std::thread::hardware_concurrency()) - 1) {
        system_.Init(65536, 0, 65536, 10240, broad_layers_, broad_filter_, pair_filter_);
        system_.SetGravity({0.0F, 0.0F, -9.81F});
        // Jolt defaults to the larger restitution value. PhysX and the virtual-world
        // material model use an average, which prevents a moderately elastic prim
        // from making an otherwise inelastic terrain contact feel like rubber.
        system_.SetCombineRestitution([](const JPH::Body& first, const JPH::SubShapeID&,
                                         const JPH::Body& second, const JPH::SubShapeID&) {
            return 0.5F * (first.GetRestitution() + second.GetRestitution());
        });
    }

    BodyId create_body(const BodyDefinition& definition) override {
        JPH::BodyCreationSettings settings(make_shape(definition.shape), vec(definition.position),
            quat(definition.rotation), motion(definition.motion),
            definition.motion == MotionType::Static ? Layers::static_body : Layers::moving_body);
        settings.mFriction = static_cast<float>(definition.friction);
        settings.mRestitution = static_cast<float>(definition.restitution);
        settings.mGravityFactor = static_cast<float>(definition.gravity_multiplier);
        if (definition.motion == MotionType::Dynamic && definition.mass > 0) {
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = static_cast<float>(definition.mass);
        }
        // The region takes one collision step per 45 Hz tick, so a fast-falling
        // body can cover more than its own height in a single step, sink into the
        // terrain, and get pushed back out over the following frames — visible as
        // the object "squashing" on impact, and it swallows energy that should
        // have returned as bounce. LinearCast sweeps the motion instead.
        if (definition.motion == MotionType::Dynamic)
            settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
        const auto native = system_.GetBodyInterface().CreateAndAddBody(settings,
            definition.motion == MotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        if (native.IsInvalid()) throw std::runtime_error("Jolt could not create a body");
        system_.GetBodyInterface().SetLinearVelocity(native, vec(definition.velocity));
        const auto id = next_body_++;
        bodies_.emplace(id, JoltBody{native, definition.entity_id});
        native_to_body_.emplace(native.GetIndexAndSequenceNumber(), id);
        return id;
    }

    bool remove_body(BodyId id) override {
        const auto found = bodies_.find(id);
        if (found == bodies_.end()) return false;
        auto& interface = system_.GetBodyInterface();
        interface.RemoveBody(found->second.native);
        interface.DestroyBody(found->second.native);
        native_to_body_.erase(found->second.native.GetIndexAndSequenceNumber());
        bodies_.erase(found);
        return true;
    }

    std::optional<BodyState> body_state(BodyId id) const override {
        const auto found = bodies_.find(id);
        if (found == bodies_.end()) return std::nullopt;
        const auto& interface = system_.GetBodyInterface();
        const auto rotation = interface.GetRotation(found->second.native);
        return BodyState{id, found->second.entity, vec(interface.GetPosition(found->second.native)),
            vec(interface.GetLinearVelocity(found->second.native)),
            vec(interface.GetAngularVelocity(found->second.native)), !interface.IsActive(found->second.native), false,
            {rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW()}};
    }

    void set_body_state(const BodyState& state) override {
        const auto found = bodies_.find(state.body_id);
        if (found == bodies_.end()) return;
        auto& interface = system_.GetBodyInterface();
        interface.SetPositionAndRotation(found->second.native, vec(state.position), quat(state.rotation),
                                         JPH::EActivation::Activate);
        interface.SetLinearAndAngularVelocity(found->second.native, vec(state.linear_velocity), vec(state.angular_velocity));
        if (state.sleeping) interface.DeactivateBody(found->second.native);
    }

    void apply_impulse(BodyId id, scene::Vector3 impulse) override {
        if (const auto found = bodies_.find(id); found != bodies_.end())
            system_.GetBodyInterface().AddImpulse(found->second.native, vec(impulse));
    }

    BodyId create_heightfield(const HeightFieldDefinition& definition) override {
        const auto count = definition.sample_count;
        if (count < 4 || definition.samples.size() != static_cast<std::size_t>(count) * count ||
            !std::isfinite(definition.spacing) || definition.spacing <= 0.0)
            throw std::invalid_argument("invalid Jolt heightfield definition");
        std::vector<float> reversed(definition.samples.size());
        for (std::uint32_t y = 0; y < count; ++y)
            std::copy_n(definition.samples.begin() + static_cast<std::size_t>(count - 1 - y) * count,
                        count, reversed.begin() + static_cast<std::size_t>(y) * count);
        JPH::HeightFieldShapeSettings shape_settings(
            reversed.data(), JPH::Vec3::sZero(),
            {static_cast<float>(definition.spacing), 1.0F, static_cast<float>(definition.spacing)}, count);
        const auto shape = shape_settings.Create();
        if (shape.HasError()) throw std::runtime_error(shape.GetError().c_str());
        const auto rotation = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), JPH::DegreesToRadians(90.0F));
        const JPH::RVec3 position{
            static_cast<float>(definition.origin.x),
            static_cast<float>(static_cast<double>(count - 1) * definition.spacing +
                               definition.origin.y),
            static_cast<float>(definition.origin.z)};
        JPH::BodyCreationSettings settings(
            shape.Get(), position, rotation, JPH::EMotionType::Static, Layers::static_body);
        // The same ghost-contact remedy for bodies rather than characters: a
        // rolling or sliding prim crosses the same internal triangle edges an
        // avatar does, and the character-side flag does not cover body-vs-body
        // contacts.
        settings.mEnhancedInternalEdgeRemoval = true;
        settings.mFriction = static_cast<float>(definition.friction);
        settings.mRestitution = static_cast<float>(definition.restitution);
        const auto native = system_.GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        if (native.IsInvalid()) throw std::runtime_error("Jolt could not create a heightfield");
        const auto id = next_body_++;
        bodies_.emplace(id, JoltBody{native, definition.entity_id});
        native_to_body_.emplace(native.GetIndexAndSequenceNumber(), id);
        return id;
    }

    CharacterId create_character(const CharacterDefinition& definition) override {
        JPH::CharacterVirtualSettings settings;
        settings.mUp = JPH::Vec3::sAxisZ();
        settings.mMaxSlopeAngle = JPH::DegreesToRadians(
            static_cast<float>(definition.walkable_slope_degrees));
        settings.mSupportingVolume = JPH::Plane(
            JPH::Vec3::sAxisZ(), -static_cast<float>(definition.radius));
        settings.mShape = make_character_shape(definition.radius, definition.height);
        settings.mMass = static_cast<float>(std::max(1.0, definition.mass));
        settings.mMaxStrength = static_cast<float>(std::max(
            0.0, definition.mass * definition.maximum_horizontal_acceleration));
        scene::Vector3 feet{
            definition.position.x, definition.position.y,
            definition.position.z - definition.height * 0.5};
        // Match Halcyon's mature sky-platform login behavior: if the restored
        // capsule overlaps scene geometry, search upward only for the nearest
        // clear creation point. A fixed offset can still leave a capsule inside
        // thick geometry or unnecessarily move avatars that were already clear.
        const auto overlaps = [&](const scene::Vector3& candidate) {
            JPH::CollideShapeSettings collide_settings;
            JPH::ClosestHitCollisionCollector<JPH::CollideShapeCollector> collector;
            const auto center_of_mass = JPH::RVec3(vec(candidate)) +
                JPH::RVec3(settings.mShape->GetCenterOfMass());
            system_.GetNarrowPhaseQuery().CollideShape(
                settings.mShape, JPH::Vec3::sOne(),
                JPH::RMat44::sTranslation(center_of_mass), collide_settings,
                JPH::RVec3::sZero(), collector);
            // Jolt can report exact surface contact as a zero-depth hit. That is
            // already a valid standing position and must not trigger a login lift.
            return collector.HadHit() && collector.mHit.mPenetrationDepth > 0.001F;
        };
        if (overlaps(feet)) {
            constexpr int maximum_attempts = 8;
            constexpr double push_multiplier = 1.5;
            double push = 0.1;
            auto candidate = feet;
            bool found_clear = false;
            for (int attempt = 0; attempt < maximum_attempts; ++attempt) {
                candidate.z += push;
                if (!overlaps(candidate)) {
                    found_clear = true;
                    break;
                }
                push *= push_multiplier;
            }
            if (found_clear) feet = candidate;
        }
        auto character = JPH::Ref<JPH::CharacterVirtual>(new JPH::CharacterVirtual(
            &settings, JPH::RVec3(vec(feet)),
            JPH::Quat::sIdentity(), &system_));
        // A capsule crossing the internal edges of a triangulated surface picks
        // up contacts against the edges themselves, whose normals are not the
        // surface normal. On terrain that shows as a walking avatar riding a few
        // centimetres off the ground it rests on, in an amount that depends on
        // which way it crosses the triangles - measured by the client core as a
        // direction-dependent residual on the operator's slope, larger downhill
        // than uphill and absent at rest (2026-07-30). This is Jolt's remedy for
        // exactly that, and it costs contact-resolution work rather than
        // correctness.
        character->SetEnhancedInternalEdgeRemoval(true);
        // Read back rather than assume, and say so once. A setting with no
        // effect and a setting that never took are indistinguishable from
        // outside the process, and the client core declined to accept a
        // falsification that could not tell them apart - correctly. This makes
        // the experiment's precondition observable instead of asserted
        // (2026-07-30).
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::cout << "{\"level\":\"info\",\"message\":\"character contact settings\""
                         ",\"enhancedInternalEdgeRemoval\":"
                      << (character->GetEnhancedInternalEdgeRemoval() ? "true" : "false")
                      << ",\"maxSlopeDegrees\":" << character_walkable_slope_degrees
                      << ",\"radius\":" << definition.radius
                      << ",\"height\":" << definition.height << "}" << std::endl;
        }
        const auto id = next_character_++;
        characters_.emplace(id, JoltCharacter{
            std::move(character), definition.entity_id, definition.height, definition.step_height, false});
        return id;
    }
    bool remove_character(CharacterId id) override {
        const auto found = characters_.find(id);
        if (found == characters_.end()) return false;
        characters_.erase(found);
        return true;
    }
    std::optional<BodyState> character_state(CharacterId id) const override {
        const auto found = characters_.find(id);
        if (found == characters_.end()) return std::nullopt;
        auto position = vec(found->second.character->GetPosition());
        position.z += found->second.height * 0.5;
        auto velocity = vec(found->second.character->GetLinearVelocity());
        // Walkable ground, not merely contact. Jolt's IsSupported() is true for
        // OnSteepGround as well as OnGround, and OnSteepGround means the slope
        // is past mMaxSlopeAngle — its own documentation says "the caller should
        // start applying downward velocity if sliding from the slope is
        // desired". Reporting that as grounded, and then zeroing the downward
        // velocity below, did the exact opposite: it pinned an avatar motionless
        // on ground far steeper than it could ever walk, which is what an
        // operator measured repeatably at 70 degrees.
        //
        // Treating only OnGround as grounded costs no new published constant and
        // no new state. An avatar on too-steep ground is simply not supported,
        // so gravity applies and the existing fall path carries it — which is
        // what the first-party client asked for, having already implemented and
        // tested falls while a slide would have been a new term for it to
        // predict.
        //
        // The stick-to-floor logic in update() deliberately keeps using
        // IsSupported(): it exists to stop a character going airborne while
        // walking downhill, and OnGround to OnSteepGround leaves it true, so a
        // slope that becomes too steep is not stuck to.
        const auto grounded =
            found->second.character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        if (grounded && velocity.z < 0.0) velocity.z = 0.0;
        return BodyState{id, found->second.entity, position,
                         velocity, {}, false, grounded};
    }
    void set_character_state(CharacterId id, const BodyState& state) override {
        const auto found = characters_.find(id);
        if (found == characters_.end()) return;
        auto feet = state.position;
        feet.z -= found->second.height * 0.5;
        found->second.character->SetPosition(JPH::RVec3(vec(feet)));
        found->second.character->SetLinearVelocity(vec(state.linear_velocity));
    }
    void set_character_velocity(CharacterId id, scene::Vector3 velocity) override {
        if (const auto found = characters_.find(id); found != characters_.end())
            found->second.character->SetLinearVelocity(vec(velocity));
    }
    void set_character_flying(CharacterId id, bool flying) override {
        if (const auto found = characters_.find(id); found != characters_.end())
            found->second.flying = flying;
    }

    void step(double seconds) override {
        contacts_.clear();
        for (auto& [id, entry] : characters_) {
            static_cast<void>(id);
            update_character(entry, static_cast<float>(seconds));
        }
        system_.Update(static_cast<float>(seconds), 1, &allocator_, &jobs_);
    }
    std::span<const Contact> contacts() const override { return contacts_; }

    std::optional<RayHit> ray_cast(scene::Vector3 origin, scene::Vector3 direction,
                                   double maximum_distance) const override {
        JPH::RRayCast ray{JPH::RVec3(vec(origin)), vec(direction).Normalized() * static_cast<float>(maximum_distance)};
        JPH::RayCastResult hit;
        if (!system_.GetNarrowPhaseQuery().CastRay(ray, hit)) return std::nullopt;
        const auto found = native_to_body_.find(hit.mBodyID.GetIndexAndSequenceNumber());
        if (found == native_to_body_.end()) return std::nullopt;
        const auto point = ray.GetPointOnRay(hit.mFraction);
        return RayHit{found->second, vec(point), {}, hit.mFraction};
    }

    std::optional<RayHit> ray_cast_body(BodyId id, scene::Vector3 origin,
                                        scene::Vector3 direction,
                                        double maximum_distance) const override {
        const auto target = bodies_.find(id);
        if (target == bodies_.end()) return std::nullopt;
        class TargetBodyFilter final : public JPH::BodyFilter {
        public:
            explicit TargetBodyFilter(JPH::BodyID target) : target_(target) {}
            bool ShouldCollide(const JPH::BodyID& candidate) const override {
                return candidate == target_;
            }
        private:
            JPH::BodyID target_;
        } filter(target->second.native);
        JPH::RRayCast ray{
            JPH::RVec3(vec(origin)), vec(direction).Normalized() * static_cast<float>(maximum_distance)};
        JPH::RayCastResult hit;
        if (!system_.GetNarrowPhaseQuery().CastRay(ray, hit, {}, {}, filter)) return std::nullopt;
        const auto point = ray.GetPointOnRay(hit.mFraction);
        return RayHit{id, vec(point), {}, hit.mFraction};
    }

    TransferState capture(std::span<const BodyId> ids) const override {
        TransferState result;
        for (const auto id : ids) if (const auto state = body_state(id)) result.bodies.push_back(*state);
        return result;
    }
    void restore(const TransferState& state) override {
        for (const auto& body : state.bodies) set_body_state(body);
    }

private:
    void update_character(JoltCharacter& entry, float seconds) {
        auto& character = *entry.character;
        const JPH::Vec3 gravity{0, 0, -9.81F};
        const JPH::Vec3 stick_down = entry.flying ? JPH::Vec3::sZero() : JPH::Vec3{0, 0, -0.5F};
        const JPH::Vec3 step_up = entry.flying ? JPH::Vec3::sZero() :
            JPH::Vec3{0, 0, static_cast<float>(entry.step_height)};
        character.StartTrackingContactChanges();
        const auto desired_velocity = character.GetLinearVelocity();
        character.SetLinearVelocity(character.CancelVelocityTowardsSteepSlopes(desired_velocity));
        const auto old_position = character.GetPosition();
        auto ground_to_air = character.IsSupported();
        character.Update(seconds, gravity, {}, {}, {}, {}, allocator_);
        if (character.IsSupported()) ground_to_air = false;
        if (ground_to_air && !stick_down.IsNearZero()) {
            const auto vertical_velocity = JPH::Vec3(character.GetPosition() - old_position)
                .Dot(JPH::Vec3::sAxisZ()) / seconds;
            if (vertical_velocity <= 1.0e-6F)
                character.StickToFloor(stick_down, {}, {}, {}, {}, allocator_);
        }

        const auto hit_dynamic_body = std::any_of(
            character.GetActiveContacts().begin(), character.GetActiveContacts().end(),
            [](const JPH::CharacterVirtual::Contact& contact) {
                return contact.mHadCollision &&
                       contact.mMotionTypeB == JPH::EMotionType::Dynamic;
            });
        if (!step_up.IsNearZero() && !hit_dynamic_body) {
            auto desired_step = desired_velocity * seconds;
            desired_step -= desired_step.Dot(JPH::Vec3::sAxisZ()) * JPH::Vec3::sAxisZ();
            const auto desired_length = desired_step.Length();
            if (desired_length > 0.0F) {
                auto achieved_step = JPH::Vec3(character.GetPosition() - old_position);
                achieved_step -= achieved_step.Dot(JPH::Vec3::sAxisZ()) * JPH::Vec3::sAxisZ();
                const auto forward = desired_step / desired_length;
                achieved_step = std::max(0.0F, achieved_step.Dot(forward)) * forward;
                const auto achieved_length = achieved_step.Length();
                if (achieved_length + 1.0e-4F < desired_length &&
                    character.CanWalkStairs(desired_velocity)) {
                    const auto step_forward = forward * std::max(0.02F, desired_length - achieved_length);
                    auto step_test = -character.GetGroundNormal();
                    step_test -= step_test.Dot(JPH::Vec3::sAxisZ()) * JPH::Vec3::sAxisZ();
                    step_test = step_test.NormalizedOr(forward);
                    if (step_test.Dot(forward) < std::cos(75.0F * 3.14159265358979323846F / 180.0F))
                        step_test = forward;
                    character.WalkStairs(seconds, step_up, step_forward, step_test * 0.15F, {},
                                         {}, {}, {}, {}, allocator_);
                }
            }
        }
        character.FinishTrackingContactChanges();
    }

    BroadPhaseLayers broad_layers_;
    ObjectBroadPhaseFilter broad_filter_;
    ObjectPairFilter pair_filter_;
    JPH::TempAllocatorImpl allocator_;
    JPH::JobSystemThreadPool jobs_;
    JPH::PhysicsSystem system_;
    BodyId next_body_{1};
    CharacterId next_character_{1};
    std::unordered_map<BodyId, JoltBody> bodies_;
    std::unordered_map<JPH::uint32, BodyId> native_to_body_;
    std::unordered_map<CharacterId, JoltCharacter> characters_;
    std::vector<Contact> contacts_;
};

} // namespace

std::unique_ptr<World> make_jolt_world() {
    initialize_jolt();
    return std::make_unique<JoltWorld>();
}

} // namespace homeworldz::physics
