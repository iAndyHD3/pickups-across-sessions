
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <ghc/filesystem.hpp>
#include <span>

using namespace geode;
using namespace cocos2d;
using namespace geode::log;
namespace fs = ghc::filesystem;
template<typename T> using Arr = geode::cocos::CCArrayExt<T>;
using GameObjArr = Arr<GameObject*>;

inline auto playlayer() {return GameManager::sharedState()->m_playLayer;}
inline auto editor() {return GameManager::sharedState()->m_levelEditorLayer;}

template<typename T>
inline T& from_offset(void* self, uintptr_t offset) {
	return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(self) + offset);
}

gd::unordered_map<int, int>& getRobtopsItemIdsMap()
{
#if defined(GEODE_IS_WINDOWS)
	auto self = GameManager::sharedState()->getPlayLayer()->m_effectManager;
	return from_offset<gd::unordered_map<int, int>>(self, 512);
#else
	#error add some offset here
#endif
}

constexpr int INVALID_INT = -1;

bool isNumeric(std::string_view str)
{
	for(const auto& c : str)
		if(!std::isdigit(c))
			return false;
	return true;
}

//@{key} {value}
std::optional<std::string_view> getValueFromText(std::string_view line, std::string_view key)
{
    constexpr auto npos = std::string_view::npos;
    if(line[0] != '@') return {}; //return early if not comment (avoid format)

    auto start = line.find(fmt::format("@{}", key).c_str());
    if (start == npos) return {};

    auto startValue = line.find_first_of(' ', start);
    if (startValue == npos) return {};

    return line.substr(startValue + 1, line.size() - startValue);
};

std::optional<int> getNumericValueFromText(std::string_view line, std::string_view key)
{
	auto val = getValueFromText(line, key);
	if(!val) return {};
	return geode::utils::numFromString<int>(*val).ok();
}

std::optional<std::pair<int, std::string_view>> getValueFromTextNumericValue(std::string_view line)
{
    constexpr auto npos = std::string_view::npos;
    if(line[0] != '@') return {};

    auto startValue = line.find_first_of(' ');
    if (startValue == npos) return {};

    auto key = line.substr(1, startValue - 1);
    if(!isNumeric(key)) return {};

    auto value = line.substr(startValue + 1);

	if(auto key_i = geode::utils::numFromString<int>(key); key_i.isOk())
	{
	    decltype(getValueFromTextNumericValue(line)) ret;
		ret.emplace(key_i.unwrap(), value);
		return ret;
	}
	return {};
};

struct PersistentID
{
	std::string key;
	int itemID = 0;
	bool operator<(const PersistentID& b) const { return itemID < b.itemID; };
};

struct PersPickup
{
	struct Members
	{
		bool mod_enabled = false;
		bool hasLoadedOnce = false;
		int accID = INVALID_INT;
		int restoreGroup = INVALID_INT;
		int saveGroup = INVALID_INT;
		int applyGroup = INVALID_INT;
		int enabledGroup = INVALID_INT;

 		//key <-> item ID, NOT VALUE
		//this is loaded from text objects when the level starts
		std::set<PersistentID> persistentIds;
		gd::unordered_map<int, int> temp_died;

	} m;

	std::optional<int> persistentItemIdForKey(std::string_view key)
	{
		for(const auto& persid : m.persistentIds)
			if(persid.key == key)
				return persid.itemID;
		return {};
	}

	static std::optional<int> getAccIdFromLevel(GJGameLevel* level)
	{
		int id = [&](){
			if(level->m_levelType == GJLevelType::Editor) return GJAccountManager::sharedState()->m_accountID;
			else return level->m_accountID.value();
		}();

		return id >= 1 ? std::optional(id) : std::nullopt;
	}

	void tryLoadLevel(PlayLayer* pl, GJGameLevel* level)
	{
		auto optAccId = getAccIdFromLevel(level);
		if(!optAccId) return error("{}", "Could not get account id of the level");

		m.hasLoadedOnce = true;
		m.mod_enabled = false;
		auto textObjects = getAllTexts(pl);
		for(const auto& str : textObjects)
		{
			if(str == "@perspickup")
			{
				m.mod_enabled = true;
				break;
			}
		}
		if(!m.mod_enabled) return log::error("text @perspickup not found");

		m.accID = optAccId.value();
		loadFromFile(textObjects);
	}

	void saveFileAndReset()
	{
		saveToFile();
		resetMembers();
	}
	void resetMembers()
	{
		m.accID = INVALID_INT;
		m.restoreGroup = INVALID_INT;
		m.saveGroup = INVALID_INT;
		m.applyGroup = INVALID_INT;
		m.enabledGroup = INVALID_INT;
		m.persistentIds.clear();
	}

	static PersPickup& get()
	{
		static PersPickup instance;
		return instance;
	}

	void loadSpecialGroupIds(std::span<std::string_view> textObjects)
	{
		for(const auto& str : textObjects)
		{
			if(auto v = getNumericValueFromText(str, "save")) m.saveGroup = *v;
			if(auto v = getNumericValueFromText(str, "apply")) m.applyGroup = *v;
			if(auto v = getNumericValueFromText(str, "restore")) m.restoreGroup = *v;
		}
	}

	static std::vector<std::string_view> getAllTexts(PlayLayer* pl)
	{
		decltype(getAllTexts(pl)) ret;

		for(const auto& obj : GameObjArr(pl->m_objects))
		{
			if(obj->m_objectID != 914) continue;
			ret.emplace_back(static_cast<TextGameObject*>(obj)->m_text);
		}
		return ret;
	}

	[[nodiscard]] fs::path getFilePathAccID() 
	{
		if(m.accID == INVALID_INT) return {};
		return geode::dirs::getModsSaveDir() /= fmt::format("{}-itemids.json", m.accID);
	}

	void playerBeforeDying()
	{
		for(const auto& ids : getRobtopsItemIdsMap())
			m.temp_died.insert_or_assign(ids.first, ids.second);
	}

	void playerDied()
	{
		auto pl = playlayer();
		auto& map = getRobtopsItemIdsMap();
		for(const auto& persid : m.persistentIds)
		{
			auto savedID = m.temp_died.find(persid.itemID);
			if(savedID == map.end()) continue;

			int id = savedID->first;
			int val = savedID->second;

			map.insert_or_assign(id, val);
			pl->updateCounters(id, val);
		}
		m.temp_died.clear();
	}

	void loadPersistentItemIds(std::span<std::string_view> textObjects)
	{
		for(const auto& str : textObjects)
		{
			if(auto pair = getValueFromTextNumericValue(str))
			{
				m.persistentIds.insert(PersistentID{.key = std::string(pair->second), .itemID = pair->first});
			}
		}
	}

	bool loadFromFile(std::span<std::string_view> textObjects)
	{
		loadPersistentItemIds(textObjects);

		auto path = getFilePathAccID();
		std::ifstream file(path);
		if(!file) return false;

		std::string str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		std::string jsonError;
		auto jsonopt = matjson::Value::from_str(str, jsonError);
		if(!jsonopt)
		{
			geode::log::error("Error parsing json: {}", jsonError);
			return false;
		}

		const matjson::Value& json = jsonopt.value();
		if(!json.is_object()) return false;


		auto pl = playlayer();
		auto& gdmap = getRobtopsItemIdsMap();

        for(const auto& savedItem : json.as_object())
		{
			if(!savedItem.second.is_number()) continue;

			auto itemid = persistentItemIdForKey(savedItem.first);
			if(!itemid) continue;

			gdmap.insert_or_assign(itemid.value(), savedItem.second.as_int());
			pl->updateCounters(
				itemid.value(), //item id specified in level text object
				savedItem.second.as_int() //item value saved in json
			);
		}
		return true;
	}

	bool saveToFile()
	{
		auto path = getFilePathAccID();
		std::ofstream f(path);
		if(!f.good()) return false;
		matjson::Object holder;

		for(const auto& persistentId : m.persistentIds)
		{
			auto gdmap = getRobtopsItemIdsMap();
			auto it = gdmap.find(persistentId.itemID);
			if(it == gdmap.end()) continue;

			holder.insert({std::string(persistentId.key), it->second});
		}
		
		if(holder.empty())
		{
			geode::log::error("Could not find any values to save, something wrong probably happened");
			return false;
		}

		auto jsonstr = matjson::Value(holder).dump(4);
		f << jsonstr;
		geode::log::info("Saving: {}", jsonstr);
		geode::log::info("At \"{}\"", path.string());
		return true;
	}
};


struct MyPlayLayer : Modify<MyPlayLayer, PlayLayer>
{
	void setupHasCompleted()
	{
		PlayLayer::setupHasCompleted();

		PersPickup::get().tryLoadLevel(this, m_level);
	}

	void pauseGame(bool a)
	{
		PlayLayer::pauseGame(a);
		for(const auto& o : from_offset<gd::unordered_map<int, int>>(m_effectManager, 512))
		{
			geode::log::info("{} {}", o.first, o.second);
		}
	}
	void onQuit()
	{
		PlayLayer::onQuit();
		if(auto p = PersPickup::get(); p.m.mod_enabled)
		{
			p.saveFileAndReset();
		}
	}

	void resetLevel()
	{
		if(auto& p = PersPickup::get(); p.m.hasLoadedOnce && p.m.mod_enabled)
		{
			p.playerBeforeDying();
			PlayLayer::resetLevel();
			p.playerDied();
		}
		else
		{
			PlayLayer::resetLevel();
		}
	}
};