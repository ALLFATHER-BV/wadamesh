#include "FriendMeshNavigation.h"

#include <math.h>
#include <string.h>

namespace friendmesh {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusMeters = 6371000.0;
constexpr uint32_t kMotionGoodDisplacementMeters = 15;
constexpr double kMotionMaxMetersPerSecond = 3.5;

double radiansFromE7(int32_t value) {
  return (static_cast<double>(value) / 10000000.0) * kPi / 180.0;
}

uint32_t roundedMeters(double value) {
  if (value <= 0.0) return 0;
  if (value >= 4294967295.0) return UINT32_MAX;
  return static_cast<uint32_t>(value + 0.5);
}

bool memberAllowed(const DomainStore& domain, const EventHeader& event) {
  const GroupRecord* group = domain.groupById(event.groupId);
  const GroupMember* member = domain.groupMember(event.groupId, event.senderId);
  const FriendRecord* sender = domain.friendById(event.senderId);
  return group && member && sender && !sender->blockedLocally &&
         group->security == GroupSecurityState::ReadyForDevelopment &&
         event.membershipEpoch == group->membershipEpoch &&
         member->state == MemberState::Approved &&
         member->grantState == GrantState::Active &&
         member->grantedEpoch == group->membershipEpoch;
}

bool positionUsable(const PositionRecord& position) {
  return position.valid && !position.hiddenByPolicy &&
         positionCoordinatesValid(position.latitudeE7, position.longitudeE7);
}

bool meetupTerminal(MeetupState state) {
  return state == MeetupState::Completed || state == MeetupState::Cancelled ||
         state == MeetupState::Expired;
}

}  // namespace

bool positionCoordinatesValid(int32_t latitudeE7, int32_t longitudeE7) {
  return latitudeE7 >= -900000000 && latitudeE7 <= 900000000 &&
         longitudeE7 >= -1800000000 && longitudeE7 <= 1800000000;
}

uint32_t greatCircleDistanceMeters(const PositionRecord& from,
                                   const PositionRecord& to) {
  if (!positionUsable(from) || !positionUsable(to)) return UINT32_MAX;
  const double lat1 = radiansFromE7(from.latitudeE7);
  const double lat2 = radiansFromE7(to.latitudeE7);
  const double deltaLat = lat2 - lat1;
  const double deltaLon = radiansFromE7(to.longitudeE7 - from.longitudeE7);
  const double sinLat = sin(deltaLat / 2.0);
  const double sinLon = sin(deltaLon / 2.0);
  double a = sinLat * sinLat + cos(lat1) * cos(lat2) * sinLon * sinLon;
  if (a < 0.0) a = 0.0;
  if (a > 1.0) a = 1.0;
  return roundedMeters(kEarthRadiusMeters * 2.0 * atan2(sqrt(a), sqrt(1.0 - a)));
}

uint16_t absoluteBearingDegrees(const PositionRecord& from,
                                const PositionRecord& to) {
  if (!positionUsable(from) || !positionUsable(to)) return 0;
  const double lat1 = radiansFromE7(from.latitudeE7);
  const double lat2 = radiansFromE7(to.latitudeE7);
  const double deltaLon = radiansFromE7(to.longitudeE7 - from.longitudeE7);
  const double y = sin(deltaLon) * cos(lat2);
  const double x = cos(lat1) * sin(lat2) -
                   sin(lat1) * cos(lat2) * cos(deltaLon);
  double degrees = atan2(y, x) * 180.0 / kPi;
  if (degrees < 0.0) degrees += 360.0;
  return static_cast<uint16_t>(degrees + 0.5) % 360;
}

MotionEstimate estimateTargetMotion(const PositionRecord& previous,
                                    const PositionRecord& current,
                                    uint32_t now,
                                    uint32_t horizonSeconds) {
  MotionEstimate estimate = {};
  if (!positionUsable(previous) || !positionUsable(current) ||
      !idsEqual(previous.subjectId, current.subjectId) ||
      previous.capturedAt == 0 || current.capturedAt <= previous.capturedAt ||
      now == 0 || now < current.capturedAt ||
      now - current.capturedAt > kDefaultPositionStaleSeconds) {
    return estimate;
  }
  const uint32_t sampleSeconds = current.capturedAt - previous.capturedAt;
  if (sampleSeconds < kMotionMinSampleSeconds ||
      sampleSeconds > kMotionMaxSampleSeconds) return estimate;

  const uint32_t displacement = greatCircleDistanceMeters(previous, current);
  if (displacement == UINT32_MAX) return estimate;
  const double speed = static_cast<double>(displacement) /
                       static_cast<double>(sampleSeconds);
  if (speed > kMotionMaxMetersPerSecond) return estimate;

  estimate.samplesUsable = true;
  estimate.displacementMeters = displacement;
  estimate.sampleSeconds = sampleSeconds;
  estimate.horizonSeconds = horizonSeconds;
  estimate.speedCentimetersPerSecond = static_cast<uint16_t>(
      speed * 100.0 + 0.5);
  estimate.predicted = current;
  if (displacement < kMotionObservationSpacingMeters || horizonSeconds == 0) {
    estimate.confidence = MotionConfidence::Limited;
    return estimate;
  }

  estimate.moving = true;
  estimate.bearingDegrees = absoluteBearingDegrees(previous, current);
  estimate.confidence = displacement >= kMotionGoodDisplacementMeters
      ? MotionConfidence::Good : MotionConfidence::Limited;

  const double travelMeters = speed * static_cast<double>(horizonSeconds);
  const double angularDistance = travelMeters / kEarthRadiusMeters;
  const double bearing = static_cast<double>(estimate.bearingDegrees) *
                         kPi / 180.0;
  const double lat1 = radiansFromE7(current.latitudeE7);
  const double lon1 = radiansFromE7(current.longitudeE7);
  const double sinLat2 = sin(lat1) * cos(angularDistance) +
      cos(lat1) * sin(angularDistance) * cos(bearing);
  double lat2 = asin(sinLat2);
  double lon2 = lon1 + atan2(
      sin(bearing) * sin(angularDistance) * cos(lat1),
      cos(angularDistance) - sin(lat1) * sin(lat2));
  while (lon2 > kPi) lon2 -= 2.0 * kPi;
  while (lon2 < -kPi) lon2 += 2.0 * kPi;
  const double e7PerRadian = 180.0 * 10000000.0 / kPi;
  estimate.predicted.latitudeE7 = static_cast<int32_t>(lat2 * e7PerRadian);
  estimate.predicted.longitudeE7 = static_cast<int32_t>(lon2 * e7PerRadian);
  if (!positionCoordinatesValid(estimate.predicted.latitudeE7,
                                estimate.predicted.longitudeE7)) {
    estimate = {};
  }
  return estimate;
}

CourseToTargetEstimate estimateCourseToTarget(
    const PositionRecord& previousLocal, const PositionRecord& currentLocal,
    const PositionRecord& target, uint32_t now) {
  CourseToTargetEstimate estimate = {};
  estimate.progress = NavigationProgress::Unknown;
  estimate.motion = estimateTargetMotion(previousLocal, currentLocal, now);
  const uint32_t previousDistance =
      greatCircleDistanceMeters(previousLocal, target);
  const uint32_t currentDistance =
      greatCircleDistanceMeters(currentLocal, target);
  if (previousDistance == UINT32_MAX || currentDistance == UINT32_MAX) {
    estimate.distanceMeters = UINT32_MAX;
    return estimate;
  }
  estimate.targetUsable = true;
  estimate.distanceMeters = currentDistance;
  if (!estimate.motion.samplesUsable || estimate.motion.sampleSeconds == 0)
    return estimate;

  const int64_t closedMeters = static_cast<int64_t>(previousDistance) -
      static_cast<int64_t>(currentDistance);
  const int64_t closingCentimetersPerSecond =
      (closedMeters * 100) /
      static_cast<int64_t>(estimate.motion.sampleSeconds);
  if (closingCentimetersPerSecond > INT32_MAX)
    estimate.closingSpeedCentimetersPerSecond = INT32_MAX;
  else if (closingCentimetersPerSecond < INT32_MIN)
    estimate.closingSpeedCentimetersPerSecond = INT32_MIN;
  else
    estimate.closingSpeedCentimetersPerSecond =
        static_cast<int32_t>(closingCentimetersPerSecond);

  if (currentDistance <= kArrivalDistanceMeters) {
    estimate.progress = NavigationProgress::Arrived;
  } else if (closedMeters > static_cast<int64_t>(kProgressChangeMeters)) {
    estimate.progress = NavigationProgress::Closer;
  } else if (closedMeters < -static_cast<int64_t>(kProgressChangeMeters)) {
    estimate.progress = NavigationProgress::Farther;
  } else {
    estimate.progress = NavigationProgress::Steady;
  }

  if (!estimate.motion.moving) return estimate;
  const uint16_t targetBearing =
      absoluteBearingDegrees(currentLocal, target);
  int turn = static_cast<int>(targetBearing) -
      static_cast<int>(estimate.motion.bearingDegrees);
  while (turn > 180) turn -= 360;
  while (turn < -180) turn += 360;
  estimate.turnDegrees = static_cast<int16_t>(turn);
  estimate.courseUsable = true;
  return estimate;
}

PositionBook::PositionBook() : count_(0) {
  memset(records_, 0, sizeof(records_));
}

ResultCode PositionBook::setLocal(const PositionRecord& position) {
  if (idIsZero(position.subjectId) || !positionCoordinatesValid(
          position.latitudeE7, position.longitudeE7)) {
    return ResultCode::InvalidArgument;
  }
  for (size_t i = 0; i < count_; ++i) {
    if (idsEqual(records_[i].subjectId, position.subjectId)) {
      if (position.capturedAt < records_[i].capturedAt) return ResultCode::Conflict;
      records_[i] = position;
      return ResultCode::Ok;
    }
  }
  if (count_ >= kMaxPositionRecords) return ResultCode::CapacityReached;
  records_[count_++] = position;
  return ResultCode::Ok;
}

ResultCode PositionBook::apply(const EventHeader& event,
                               const PositionRecord& position,
                               const DomainStore& domain,
                               uint32_t receivedAt) {
  if (event.type != EventType::PositionShared ||
      validateEventHeader(event) != ResultCode::Ok ||
      !idsEqual(position.subjectId, event.senderId)) {
    return ResultCode::InvalidArgument;
  }
  if (!memberAllowed(domain, event)) return ResultCode::Unauthorized;
  const GroupRecord* group = domain.groupById(event.groupId);
  if (!group || group->locationVisibility != LocationVisibility::Precise) {
    return ResultCode::Unauthorized;
  }
  PositionRecord accepted = position;
  accepted.receivedAt = receivedAt;
  return setLocal(accepted);
}

const PositionRecord* PositionBook::bySubject(const Id128& subjectId) const {
  for (size_t i = 0; i < count_; ++i) {
    if (idsEqual(records_[i].subjectId, subjectId)) return &records_[i];
  }
  return nullptr;
}

bool PositionBook::stale(const Id128& subjectId, uint32_t now,
                         uint32_t staleAfter) const {
  const PositionRecord* position = bySubject(subjectId);
  return !position || !positionUsable(*position) || now < position->capturedAt ||
         now - position->capturedAt > staleAfter;
}

NavigationService::NavigationService()
    : breadcrumbCount_(0), previousDistance_(UINT32_MAX), arrived_(false) {
  memset(&state_, 0, sizeof(state_));
  memset(breadcrumbs_, 0, sizeof(breadcrumbs_));
}

ResultCode NavigationService::start(const Id128& targetId, uint32_t startedAt) {
  if (idIsZero(targetId)) return ResultCode::InvalidId;
  state_ = {};
  state_.targetId = targetId;
  state_.startedAt = startedAt;
  state_.active = true;
  state_.targetStale = true;
  state_.headingSource = HeadingSource::NorthUp;
  breadcrumbCount_ = 0;
  previousDistance_ = UINT32_MAX;
  arrived_ = false;
  return ResultCode::Ok;
}

ResultCode NavigationService::stop() {
  if (!state_.active) return ResultCode::InvalidState;
  state_.active = false;
  return ResultCode::Ok;
}

void NavigationService::addBreadcrumb(const PositionRecord& local) {
  if (!positionUsable(local)) return;
  if (breadcrumbCount_ > 0 &&
      greatCircleDistanceMeters(breadcrumbs_[breadcrumbCount_ - 1], local) <
          kBreadcrumbSpacingMeters) {
    return;
  }
  if (breadcrumbCount_ < kMaxBreadcrumbs) {
    breadcrumbs_[breadcrumbCount_++] = local;
    return;
  }
  memmove(&breadcrumbs_[0], &breadcrumbs_[1],
          sizeof(PositionRecord) * (kMaxBreadcrumbs - 1));
  breadcrumbs_[kMaxBreadcrumbs - 1] = local;
}

NavigationUpdate NavigationService::update(const PositionRecord& local,
                                            const PositionRecord* target,
                                            uint32_t now,
                                            uint16_t optionalHeadingDegrees,
                                            bool magnetometerAvailable) {
  NavigationUpdate update = {};
  update.state = state_;
  update.progress = NavigationProgress::Unknown;
  if (!state_.active || !positionUsable(local) || !target ||
      !idsEqual(target->subjectId, state_.targetId) || !positionUsable(*target)) {
    state_.targetStale = true;
    update.state = state_;
    return update;
  }
  state_.targetPosition = *target;
  state_.targetStale = now < target->capturedAt ||
                       now - target->capturedAt > kDefaultPositionStaleSeconds;
  state_.distanceMeters = greatCircleDistanceMeters(local, *target);
  state_.absoluteBearingDegrees = absoluteBearingDegrees(local, *target);
  if (magnetometerAvailable) {
    state_.headingDegrees = optionalHeadingDegrees % 360;
    state_.headingSource = HeadingSource::Magnetometer;
  } else if (breadcrumbCount_ > 0 &&
             greatCircleDistanceMeters(breadcrumbs_[breadcrumbCount_ - 1], local) >=
                 kProgressChangeMeters) {
    state_.headingDegrees = absoluteBearingDegrees(
        breadcrumbs_[breadcrumbCount_ - 1], local);
    state_.headingSource = HeadingSource::GpsCourse;
  } else {
    state_.headingDegrees = 0;
    state_.headingSource = HeadingSource::NorthUp;
  }
  const bool nowArrived = state_.distanceMeters <= kArrivalDistanceMeters;
  update.arrivalChanged = nowArrived != arrived_;
  if (nowArrived) {
    update.progress = NavigationProgress::Arrived;
  } else if (previousDistance_ == UINT32_MAX) {
    update.progress = NavigationProgress::Unknown;
  } else if (state_.distanceMeters + kProgressChangeMeters < previousDistance_) {
    update.progress = NavigationProgress::Closer;
  } else if (state_.distanceMeters > previousDistance_ + kProgressChangeMeters) {
    update.progress = NavigationProgress::Farther;
  } else {
    update.progress = NavigationProgress::Steady;
  }
  previousDistance_ = state_.distanceMeters;
  arrived_ = nowArrived;
  addBreadcrumb(local);
  update.targetAvailable = true;
  update.state = state_;
  return update;
}

const PositionRecord* NavigationService::breadcrumbAt(size_t index) const {
  return index < breadcrumbCount_ ? &breadcrumbs_[index] : nullptr;
}

MarkerService::MarkerService() : count_(0) {
  memset(markers_, 0, sizeof(markers_));
}

const MarkerRecord* MarkerService::byId(const Id128& markerId) const {
  for (size_t i = 0; i < count_; ++i) {
    if (idsEqual(markers_[i].markerId, markerId)) return &markers_[i];
  }
  return nullptr;
}

const MarkerRecord* MarkerService::at(size_t index) const {
  return index < count_ ? &markers_[index] : nullptr;
}

ResultCode MarkerService::create(const EventHeader& event,
                                 const MarkerRecord& marker,
                                 const DomainStore& domain) {
  if (event.type != EventType::MarkerCreated || idIsZero(marker.markerId) ||
      !idsEqual(marker.markerId, event.eventId) ||
      !idsEqual(marker.groupId, event.groupId) ||
      !idsEqual(marker.creatorId, event.senderId) ||
      !positionUsable(marker.position)) {
    return ResultCode::InvalidArgument;
  }
  if (!memberAllowed(domain, event)) return ResultCode::Unauthorized;
  if (byId(marker.markerId)) return ResultCode::Duplicate;
  if (count_ >= kMaxMarkers) return ResultCode::CapacityReached;
  markers_[count_++] = marker;
  return ResultCode::Ok;
}

ResultCode MarkerService::update(const EventHeader& event,
                                 const MarkerRecord& replacement,
                                 const DomainStore& domain) {
  if (event.type != EventType::MarkerUpdated ||
      !idsEqual(replacement.groupId, event.groupId) ||
      !positionUsable(replacement.position)) {
    return ResultCode::InvalidArgument;
  }
  if (!memberAllowed(domain, event)) return ResultCode::Unauthorized;
  for (size_t i = 0; i < count_; ++i) {
    if (!idsEqual(markers_[i].markerId, replacement.markerId)) continue;
    if (markers_[i].state == MarkerState::Removed ||
        markers_[i].state == MarkerState::Expired) return ResultCode::InvalidState;
    if (!idsEqual(event.senderId, markers_[i].creatorId)) {
      const GroupMember* member = domain.groupMember(event.groupId, event.senderId);
      if (!member || member->role != MemberRole::Admin) return ResultCode::Unauthorized;
    }
    markers_[i] = replacement;
    return ResultCode::Ok;
  }
  return ResultCode::NotFound;
}

ResultCode MarkerService::remove(const EventHeader& event, const Id128& markerId,
                                 const DomainStore& domain) {
  if (event.type != EventType::MarkerRemoved || !memberAllowed(domain, event)) {
    return ResultCode::Unauthorized;
  }
  for (size_t i = 0; i < count_; ++i) {
    if (!idsEqual(markers_[i].markerId, markerId)) continue;
    if (markers_[i].state == MarkerState::Removed) return ResultCode::Duplicate;
    if (!idsEqual(event.senderId, markers_[i].creatorId)) {
      const GroupMember* member = domain.groupMember(event.groupId, event.senderId);
      if (!member || member->role != MemberRole::Admin) return ResultCode::Unauthorized;
    }
    markers_[i].state = MarkerState::Removed;
    return ResultCode::Ok;
  }
  return ResultCode::NotFound;
}

size_t MarkerService::processTime(uint32_t now,
                                  uint32_t reconfirmAfterSeconds) {
  size_t changed = 0;
  for (size_t i = 0; i < count_; ++i) {
    MarkerRecord& marker = markers_[i];
    if (marker.state == MarkerState::Removed ||
        marker.state == MarkerState::Expired) continue;
    if (marker.expiresAt != 0 && now >= marker.expiresAt) {
      marker.state = MarkerState::Expired;
      ++changed;
    } else if (reconfirmAfterSeconds != 0 && now >= marker.createdAt &&
               now - marker.createdAt >= reconfirmAfterSeconds &&
               marker.state == MarkerState::Active) {
      marker.state = MarkerState::NeedsReconfirmation;
      ++changed;
    }
  }
  return changed;
}

MeetupService::MeetupService() : count_(0) {
  memset(meetups_, 0, sizeof(meetups_));
}

const MeetupEntry* MeetupService::byId(const Id128& meetupId) const {
  for (size_t i = 0; i < count_; ++i) {
    if (idsEqual(meetups_[i].meetup.meetupId, meetupId)) return &meetups_[i];
  }
  return nullptr;
}

const MeetupEntry* MeetupService::at(size_t index) const {
  return index < count_ ? &meetups_[index] : nullptr;
}

ResultCode MeetupService::propose(const EventHeader& event,
                                  const MeetupRecord& meetup,
                                  const DomainStore& domain) {
  if (event.type != EventType::MeetupProposed ||
      !idsEqual(meetup.meetupId, event.eventId) ||
      !idsEqual(meetup.groupId, event.groupId) ||
      !idsEqual(meetup.proposerId, event.senderId) ||
      meetup.state != MeetupState::Proposed || !positionUsable(meetup.position) ||
      meetup.voteClosesAt <= meetup.proposedAt ||
      (meetup.expiresAt != 0 && meetup.expiresAt <= meetup.voteClosesAt)) {
    return ResultCode::InvalidArgument;
  }
  if (!memberAllowed(domain, event)) return ResultCode::Unauthorized;
  if (byId(meetup.meetupId)) return ResultCode::Duplicate;
  if (count_ >= kMaxMeetups) return ResultCode::CapacityReached;
  MeetupEntry& next = meetups_[count_++];
  next = {};
  next.meetup = meetup;
  return ResultCode::Ok;
}

ResultCode MeetupService::vote(const EventHeader& event, const Id128& meetupId,
                               VoteChoice choice, const DomainStore& domain,
                               uint32_t receivedAt) {
  if (event.type != EventType::MeetupVote || choice == VoteChoice::Abstain ||
      !memberAllowed(domain, event)) return ResultCode::Unauthorized;
  for (size_t i = 0; i < count_; ++i) {
    MeetupEntry& entry = meetups_[i];
    if (!idsEqual(entry.meetup.meetupId, meetupId)) continue;
    if (entry.meetup.state != MeetupState::Proposed ||
        receivedAt >= entry.meetup.voteClosesAt) return ResultCode::InvalidState;
    for (size_t voteIndex = 0; voteIndex < entry.voteCount; ++voteIndex) {
      if (!idsEqual(entry.votes[voteIndex].memberId, event.senderId)) continue;
      if (entry.votes[voteIndex].choice == choice) return ResultCode::Duplicate;
      if (entry.votes[voteIndex].choice == VoteChoice::Yes) --entry.meetup.yesVotes;
      if (entry.votes[voteIndex].choice == VoteChoice::No) --entry.meetup.noVotes;
      entry.votes[voteIndex].choice = choice;
      if (choice == VoteChoice::Yes) ++entry.meetup.yesVotes;
      if (choice == VoteChoice::No) ++entry.meetup.noVotes;
      return ResultCode::Ok;
    }
    if (entry.voteCount >= kMaxGroupMembers) return ResultCode::CapacityReached;
    entry.votes[entry.voteCount].memberId = event.senderId;
    entry.votes[entry.voteCount].choice = choice;
    ++entry.voteCount;
    if (choice == VoteChoice::Yes) ++entry.meetup.yesVotes;
    if (choice == VoteChoice::No) ++entry.meetup.noVotes;
    return ResultCode::Ok;
  }
  return ResultCode::NotFound;
}

ResultCode MeetupService::setState(const EventHeader& event,
                                   const Id128& meetupId, MeetupState state,
                                   const DomainStore& domain,
                                   uint32_t receivedAt) {
  if (event.type != EventType::MeetupStateChanged ||
      !memberAllowed(domain, event)) return ResultCode::Unauthorized;
  for (size_t i = 0; i < count_; ++i) {
    MeetupEntry& entry = meetups_[i];
    if (!idsEqual(entry.meetup.meetupId, meetupId)) continue;
    if (meetupTerminal(entry.meetup.state)) return ResultCode::InvalidState;
    const GroupMember* member = domain.groupMember(event.groupId, event.senderId);
    const bool proposer = idsEqual(event.senderId, entry.meetup.proposerId);
    if (!proposer && (!member || member->role != MemberRole::Admin)) {
      return ResultCode::Unauthorized;
    }
    if (state == MeetupState::Active &&
        (receivedAt < entry.meetup.voteClosesAt ||
         entry.meetup.yesVotes <= entry.meetup.noVotes)) {
      return ResultCode::InvalidState;
    }
    entry.meetup.state = state;
    return ResultCode::Ok;
  }
  return ResultCode::NotFound;
}

ResultCode MeetupService::move(const EventHeader& event, const Id128& meetupId,
                               const PositionRecord& position,
                               const DomainStore& domain,
                               uint32_t receivedAt) {
  if (!positionUsable(position)) return ResultCode::InvalidArgument;
  const ResultCode stateResult = setState(event, meetupId, MeetupState::Active,
                                         domain, receivedAt);
  if (stateResult != ResultCode::Ok) return stateResult;
  for (size_t i = 0; i < count_; ++i) {
    if (idsEqual(meetups_[i].meetup.meetupId, meetupId)) {
      meetups_[i].meetup.position = position;
      return ResultCode::Ok;
    }
  }
  return ResultCode::NotFound;
}

ResultCode MeetupService::setAttending(const Id128& meetupId,
                                       const Id128& memberId, bool attending,
                                       const DomainStore& domain) {
  for (size_t i = 0; i < count_; ++i) {
    MeetupEntry& entry = meetups_[i];
    if (!idsEqual(entry.meetup.meetupId, meetupId)) continue;
    const GroupMember* member = domain.groupMember(entry.meetup.groupId, memberId);
    if (!member || member->state != MemberState::Approved) {
      return ResultCode::Unauthorized;
    }
    for (size_t attendee = 0; attendee < entry.attendeeCount; ++attendee) {
      if (!idsEqual(entry.attendees[attendee], memberId)) continue;
      if (attending) return ResultCode::Duplicate;
      memmove(&entry.attendees[attendee], &entry.attendees[attendee + 1],
              sizeof(Id128) * (entry.attendeeCount - attendee - 1));
      --entry.attendeeCount;
      entry.meetup.attendingCount = entry.attendeeCount;
      return ResultCode::Ok;
    }
    if (!attending) return ResultCode::NotFound;
    if (entry.attendeeCount >= kMaxGroupMembers) return ResultCode::CapacityReached;
    entry.attendees[entry.attendeeCount++] = memberId;
    entry.meetup.attendingCount = entry.attendeeCount;
    return ResultCode::Ok;
  }
  return ResultCode::NotFound;
}

size_t MeetupService::processExpiry(uint32_t now) {
  size_t changed = 0;
  for (size_t i = 0; i < count_; ++i) {
    MeetupRecord& meetup = meetups_[i].meetup;
    if (!meetupTerminal(meetup.state) && meetup.expiresAt != 0 &&
        now >= meetup.expiresAt) {
      meetup.state = MeetupState::Expired;
      ++changed;
    }
  }
  return changed;
}

}  // namespace friendmesh
