#pragma once

#include "friendmesh/core/FriendMeshDomain.h"
#include "friendmesh/core/FriendMeshFeatureModels.h"

namespace friendmesh {

constexpr size_t kMaxPositionRecords = kMaxFriends;
constexpr size_t kMaxBreadcrumbs = 24;
constexpr size_t kMaxMarkers = 32;
constexpr size_t kMaxMeetups = 16;
constexpr uint32_t kDefaultPositionStaleSeconds = 300;
constexpr uint32_t kArrivalDistanceMeters = 20;
constexpr uint32_t kProgressChangeMeters = 5;
constexpr uint32_t kBreadcrumbSpacingMeters = 25;
constexpr uint32_t kMotionPredictionSeconds = 45;
constexpr uint32_t kMotionMinSampleSeconds = 5;
constexpr uint32_t kMotionMaxSampleSeconds = 180;
constexpr uint32_t kMotionObservationSpacingMeters = 8;

enum class MotionConfidence : uint8_t {
  None = 0,
  Limited,
  Good,
};

// A deliberately conservative two-sample motion estimate. `predicted` is a
// short lead point, not a guaranteed intercept: callers must keep the current
// observed position visually distinct and fall back to it when `moving` is
// false. The bounds reject GPS jitter, implausible jumps, and stale samples.
struct MotionEstimate {
  PositionRecord predicted;
  uint32_t displacementMeters;
  uint32_t sampleSeconds;
  uint32_t horizonSeconds;
  uint16_t bearingDegrees;
  uint16_t speedCentimetersPerSecond;
  MotionConfidence confidence;
  bool samplesUsable;
  bool moving;
};

enum class NavigationProgress : uint8_t {
  Unknown = 0,
  Closer,
  Farther,
  Steady,
  Arrived,
};

// Local course-over-ground relative to one fixed target point. Positive
// turnDegrees means turn right/clockwise; negative means left. A positive
// closing speed means the user is approaching the target, while a negative
// value means the gap is opening. This is GPS-derived course, not a magnetic
// device heading.
struct CourseToTargetEstimate {
  MotionEstimate motion;
  uint32_t distanceMeters;
  int32_t closingSpeedCentimetersPerSecond;
  int16_t turnDegrees;
  NavigationProgress progress;
  bool targetUsable;
  bool courseUsable;
};

struct NavigationUpdate {
  NavigationState state;
  NavigationProgress progress;
  bool targetAvailable;
  bool arrivalChanged;
};

bool positionCoordinatesValid(int32_t latitudeE7, int32_t longitudeE7);
uint32_t greatCircleDistanceMeters(const PositionRecord& from,
                                   const PositionRecord& to);
uint16_t absoluteBearingDegrees(const PositionRecord& from,
                                const PositionRecord& to);
MotionEstimate estimateTargetMotion(
    const PositionRecord& previous, const PositionRecord& current,
    uint32_t now, uint32_t horizonSeconds = kMotionPredictionSeconds);
CourseToTargetEstimate estimateCourseToTarget(
    const PositionRecord& previousLocal, const PositionRecord& currentLocal,
    const PositionRecord& target, uint32_t now);

class PositionBook {
 public:
  PositionBook();

  ResultCode apply(const EventHeader& event, const PositionRecord& position,
                   const DomainStore& domain, uint32_t receivedAt);
  ResultCode setLocal(const PositionRecord& position);
  const PositionRecord* bySubject(const Id128& subjectId) const;
  bool stale(const Id128& subjectId, uint32_t now,
             uint32_t staleAfter = kDefaultPositionStaleSeconds) const;
  size_t size() const { return count_; }

 private:
  PositionRecord records_[kMaxPositionRecords];
  size_t count_;
};

class NavigationService {
 public:
  NavigationService();

  ResultCode start(const Id128& targetId, uint32_t startedAt);
  ResultCode stop();
  NavigationUpdate update(const PositionRecord& local,
                          const PositionRecord* target, uint32_t now,
                          uint16_t optionalHeadingDegrees = 0,
                          bool magnetometerAvailable = false);
  const NavigationState& state() const { return state_; }
  const PositionRecord* breadcrumbAt(size_t index) const;
  size_t breadcrumbCount() const { return breadcrumbCount_; }

 private:
  NavigationState state_;
  PositionRecord breadcrumbs_[kMaxBreadcrumbs];
  size_t breadcrumbCount_;
  uint32_t previousDistance_;
  bool arrived_;

  void addBreadcrumb(const PositionRecord& local);
};

class MarkerService {
 public:
  MarkerService();

  ResultCode create(const EventHeader& event, const MarkerRecord& marker,
                    const DomainStore& domain);
  ResultCode update(const EventHeader& event, const MarkerRecord& replacement,
                    const DomainStore& domain);
  ResultCode remove(const EventHeader& event, const Id128& markerId,
                    const DomainStore& domain);
  size_t processTime(uint32_t now, uint32_t reconfirmAfterSeconds);
  const MarkerRecord* byId(const Id128& markerId) const;
  const MarkerRecord* at(size_t index) const;
  size_t size() const { return count_; }

 private:
  MarkerRecord markers_[kMaxMarkers];
  size_t count_;
};

struct MeetupVoteRecord {
  Id128 memberId;
  VoteChoice choice;
};

struct MeetupEntry {
  MeetupRecord meetup;
  MeetupVoteRecord votes[kMaxGroupMembers];
  Id128 attendees[kMaxGroupMembers];
  uint8_t voteCount;
  uint8_t attendeeCount;
};

class MeetupService {
 public:
  MeetupService();

  ResultCode propose(const EventHeader& event, const MeetupRecord& meetup,
                     const DomainStore& domain);
  ResultCode vote(const EventHeader& event, const Id128& meetupId,
                  VoteChoice choice, const DomainStore& domain,
                  uint32_t receivedAt);
  ResultCode setState(const EventHeader& event, const Id128& meetupId,
                      MeetupState state, const DomainStore& domain,
                      uint32_t receivedAt);
  ResultCode move(const EventHeader& event, const Id128& meetupId,
                  const PositionRecord& position, const DomainStore& domain,
                  uint32_t receivedAt);
  ResultCode setAttending(const Id128& meetupId, const Id128& memberId,
                          bool attending, const DomainStore& domain);
  size_t processExpiry(uint32_t now);
  const MeetupEntry* byId(const Id128& meetupId) const;
  const MeetupEntry* at(size_t index) const;
  size_t size() const { return count_; }

 private:
  MeetupEntry meetups_[kMaxMeetups];
  size_t count_;
};

}  // namespace friendmesh
